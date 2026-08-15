#define _GNU_SOURCE

#include <systemd/sd-bus.h>

#include <json-c/json.h>
#include <linux/gpio.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define BUS_NAME "org.onekvm.Device.GPIO1"
#define OBJECT_PATH "/org/onekvm/Device1"
#define INTERFACE "org.onekvm.Device1"
#define DEFAULT_CONFIG_PATH "/run/onekvm/gpio-atx-default.json"
#define USER_CONFIG_PATH "/etc/onekvm/plugins/gpio-atx.json"
#define DEFAULT_POWER_SHORT_MS 800
#define DEFAULT_POWER_LONG_MS 5000
#define DEFAULT_RESET_PULSE_MS 800
#define MAXIMUM_PULSE_MS 30000
#define LED_PUBLISH_INTERVAL_USEC 50000ULL
#define MAXIMUM_CONFIG_BYTES (1024U * 1024U)
#define ERROR_SIZE 512

typedef struct {
    bool present;
    char chip[PATH_MAX];
    uint32_t offset;
    bool active_low;
    bool open_drain;
} LineConfig;

typedef struct {
    LineConfig power_button;
    LineConfig reset_button;
    LineConfig pwr_led;
    LineConfig hdd_led;
    int power_short_ms;
    int power_long_ms;
    int reset_pulse_ms;
} ATXConfig;

typedef struct {
    bool use_default;
    bool power_button_enabled;
    char power_button_chip[PATH_MAX];
    int64_t power_button_offset;
    bool power_button_active_low;
    bool power_button_open_drain;
    bool reset_button_enabled;
    char reset_button_chip[PATH_MAX];
    int64_t reset_button_offset;
    bool reset_button_active_low;
    bool reset_button_open_drain;
    bool pwr_led_enabled;
    char pwr_led_chip[PATH_MAX];
    int64_t pwr_led_offset;
    bool pwr_led_active_low;
    bool hdd_led_enabled;
    char hdd_led_chip[PATH_MAX];
    int64_t hdd_led_offset;
    bool hdd_led_active_low;
    int power_short_ms;
    int power_long_ms;
    int reset_pulse_ms;
} Settings;

typedef struct {
    bool pwr_led_present;
    bool pwr_led;
    bool hdd_led_present;
    bool hdd_led;
} ATXState;

typedef struct {
    int fd;
} GPIOLine;

typedef struct {
    ATXConfig config;
    GPIOLine power;
    GPIOLine reset;
    GPIOLine pwr_led;
    GPIOLine hdd_led;
    ATXState last_state;
    bool last_state_set;
} Controller;

typedef struct {
    Settings settings;
    Controller controller;
    bool controller_valid;
} Service;

static void set_error(char *error, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, ERROR_SIZE, format, arguments);
    va_end(arguments);
}

static void prefix_error(char *error, const char *prefix) {
    char previous[ERROR_SIZE];
    snprintf(previous, sizeof(previous), "%s", error);
    snprintf(error, ERROR_SIZE, "%s%s", prefix, previous);
}

static bool copy_string(char *destination, size_t size, const char *source, const char *name,
                        char *error) {
    size_t length = strlen(source);
    if (length >= size) {
        set_error(error, "%s is too long", name);
        return false;
    }
    memcpy(destination, source, length + 1);
    return true;
}

static bool read_file(const char *path, char **contents, int *open_error, char *error) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (open_error != NULL) *open_error = errno;
        set_error(error, "open %s: %s", path, strerror(errno));
        return false;
    }
    char *buffer = malloc(MAXIMUM_CONFIG_BYTES + 1U);
    if (buffer == NULL) {
        close(fd);
        set_error(error, "read %s: out of memory", path);
        return false;
    }
    size_t used = 0;
    while (used < MAXIMUM_CONFIG_BYTES) {
        ssize_t count = read(fd, buffer + used, MAXIMUM_CONFIG_BYTES - used);
        if (count > 0) {
            used += (size_t)count;
            continue;
        }
        if (count == 0) break;
        if (errno == EINTR) continue;
        int saved = errno;
        free(buffer);
        close(fd);
        set_error(error, "read %s: %s", path, strerror(saved));
        return false;
    }
    if (used == MAXIMUM_CONFIG_BYTES) {
        char byte;
        ssize_t count;
        do {
            count = read(fd, &byte, 1);
        } while (count < 0 && errno == EINTR);
        if (count != 0) {
            free(buffer);
            close(fd);
            set_error(error, "%s: configuration is larger than 1 MiB", path);
            return false;
        }
    }
    close(fd);
    buffer[used] = '\0';
    *contents = buffer;
    return true;
}

static bool mkdir_parents(const char *path, char *error) {
    char copy[PATH_MAX];
    if (!copy_string(copy, sizeof(copy), path, "settings path", error)) return false;
    char *slash = strrchr(copy, '/');
    if (slash == NULL || slash == copy) return true;
    *slash = '\0';
    for (char *cursor = copy + 1;; ++cursor) {
        if (*cursor != '/' && *cursor != '\0') continue;
        char saved = *cursor;
        *cursor = '\0';
        if (mkdir(copy, 0755) < 0 && errno != EEXIST) {
            set_error(error, "create %s: %s", copy, strerror(errno));
            return false;
        }
        *cursor = saved;
        if (saved == '\0') break;
    }
    return true;
}

static bool write_all(int fd, const char *payload, size_t length, char *error) {
    size_t written = 0;
    while (written < length) {
        ssize_t count = write(fd, payload + written, length - written);
        if (count > 0) {
            written += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        set_error(error, "write GPIO ATX settings: %s", strerror(errno));
        return false;
    }
    return true;
}

static bool atomic_write(const char *path, const char *payload, char *error) {
    if (!mkdir_parents(path, error)) return false;
    char temporary[PATH_MAX];
    int count = snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path);
    if (count < 0 || (size_t)count >= sizeof(temporary)) {
        set_error(error, "GPIO ATX settings path is too long");
        return false;
    }
    int fd = mkstemp(temporary);
    if (fd < 0) {
        set_error(error, "create GPIO ATX settings: %s", strerror(errno));
        return false;
    }
    bool ok = fchmod(fd, 0600) == 0;
    if (!ok) set_error(error, "set GPIO ATX settings permissions: %s", strerror(errno));
    if (ok) ok = write_all(fd, payload, strlen(payload), error);
    if (ok && fsync(fd) < 0) {
        set_error(error, "sync GPIO ATX settings: %s", strerror(errno));
        ok = false;
    }
    if (close(fd) < 0 && ok) {
        set_error(error, "close GPIO ATX settings: %s", strerror(errno));
        ok = false;
    }
    if (ok && rename(temporary, path) < 0) {
        set_error(error, "replace GPIO ATX settings: %s", strerror(errno));
        ok = false;
    }
    if (!ok) unlink(temporary);
    return ok;
}

static json_object *parse_json_object(const char *payload, char *error) {
    json_tokener *tokener = json_tokener_new();
    if (tokener == NULL) {
        set_error(error, "allocate JSON parser: out of memory");
        return NULL;
    }
    size_t length = strlen(payload);
    json_object *value = json_tokener_parse_ex(tokener, payload, (int)length);
    enum json_tokener_error parse_error = json_tokener_get_error(tokener);
    size_t parsed = json_tokener_get_parse_end(tokener);
    if (parse_error != json_tokener_success) {
        set_error(error, "invalid JSON: %s", json_tokener_error_desc(parse_error));
        json_tokener_free(tokener);
        if (value != NULL) json_object_put(value);
        return NULL;
    }
    json_tokener_free(tokener);
    while (parsed < length && strchr(" \t\r\n", payload[parsed]) != NULL) ++parsed;
    if (parsed != length) {
        set_error(error, "configuration contains trailing JSON");
        if (value != NULL) json_object_put(value);
        return NULL;
    }
    if (value == NULL || !json_object_is_type(value, json_type_object)) {
        set_error(error, "configuration must be a JSON object");
        if (value != NULL) json_object_put(value);
        return NULL;
    }
    return value;
}

static bool allowed_key(const char *key, const char *const *allowed, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        if (strcmp(key, allowed[index]) == 0) return true;
    }
    return false;
}

static bool reject_unknown_fields(json_object *object, const char *const *allowed, size_t count,
                                  const char *context, char *error) {
    json_object_object_foreach(object, key, value) {
        (void)value;
        if (!allowed_key(key, allowed, count)) {
            set_error(error, "%s: unknown field \"%s\"", context, key);
            return false;
        }
    }
    return true;
}

static bool get_bool(json_object *object, const char *name, bool *destination, char *error) {
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, name, &value)) return true;
    if (!json_object_is_type(value, json_type_boolean)) {
        set_error(error, "%s must be a boolean", name);
        return false;
    }
    *destination = json_object_get_boolean(value) != 0;
    return true;
}

static bool get_string(json_object *object, const char *name, char *destination, size_t size,
                       char *error) {
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, name, &value)) return true;
    if (!json_object_is_type(value, json_type_string)) {
        set_error(error, "%s must be a string", name);
        return false;
    }
    return copy_string(destination, size, json_object_get_string(value), name, error);
}

static bool get_integer(json_object *object, const char *name, int64_t minimum, int64_t maximum,
                        int64_t *destination, char *error) {
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, name, &value)) return true;
    if (!json_object_is_type(value, json_type_int)) {
        set_error(error, "%s must be an integer", name);
        return false;
    }
    int64_t number = json_object_get_int64(value);
    if (number < minimum || number > maximum) {
        set_error(error, "%s is outside its allowed range", name);
        return false;
    }
    *destination = number;
    return true;
}

static bool parse_line(json_object *object, const char *name, LineConfig *line, char *error) {
    json_object *value = NULL;
    memset(line, 0, sizeof(*line));
    if (!json_object_object_get_ex(object, name, &value) ||
        json_object_is_type(value, json_type_null)) {
        return true;
    }
    if (!json_object_is_type(value, json_type_object)) {
        set_error(error, "%s must be an object", name);
        return false;
    }
    static const char *const allowed[] = {"chip", "offset", "active_low", "open_drain"};
    int64_t offset = 0;
    if (!reject_unknown_fields(value, allowed, sizeof(allowed) / sizeof(allowed[0]), name, error) ||
        !get_string(value, "chip", line->chip, sizeof(line->chip), error) ||
        !get_integer(value, "offset", 0, UINT32_MAX, &offset, error) ||
        !get_bool(value, "active_low", &line->active_low, error) ||
        !get_bool(value, "open_drain", &line->open_drain, error)) {
        prefix_error(error, ": ");
        prefix_error(error, name);
        return false;
    }
    line->present = true;
    line->offset = (uint32_t)offset;
    return true;
}

static bool parse_atx_config(const char *payload, ATXConfig *config, char *error) {
    memset(config, 0, sizeof(*config));
    json_object *object = parse_json_object(payload, error);
    if (object == NULL) return false;
    static const char *const allowed[] = {
        "power_button", "reset_button", "pwr_led", "hdd_led", "power_short_ms",
        "power_long_ms", "reset_pulse_ms",
    };
    int64_t power_short = 0;
    int64_t power_long = 0;
    int64_t reset_pulse = 0;
    bool ok = reject_unknown_fields(object, allowed, sizeof(allowed) / sizeof(allowed[0]),
                                    "ATX configuration", error) &&
              parse_line(object, "power_button", &config->power_button, error) &&
              parse_line(object, "reset_button", &config->reset_button, error) &&
              parse_line(object, "pwr_led", &config->pwr_led, error) &&
              parse_line(object, "hdd_led", &config->hdd_led, error) &&
              get_integer(object, "power_short_ms", INT_MIN, INT_MAX, &power_short, error) &&
              get_integer(object, "power_long_ms", INT_MIN, INT_MAX, &power_long, error) &&
              get_integer(object, "reset_pulse_ms", INT_MIN, INT_MAX, &reset_pulse, error);
    json_object_put(object);
    if (!ok) return false;
    config->power_short_ms = (int)power_short;
    config->power_long_ms = (int)power_long;
    config->reset_pulse_ms = (int)reset_pulse;
    return true;
}

static bool parse_settings(const char *payload, Settings *settings, char *error) {
    memset(settings, 0, sizeof(*settings));
    json_object *object = parse_json_object(payload, error);
    if (object == NULL) return false;
    static const char *const allowed[] = {
        "use_default", "power_button_enabled", "power_button_chip", "power_button_offset",
        "power_button_active_low", "power_button_open_drain", "reset_button_enabled",
        "reset_button_chip", "reset_button_offset", "reset_button_active_low",
        "reset_button_open_drain", "pwr_led_enabled", "pwr_led_chip", "pwr_led_offset",
        "pwr_led_active_low", "hdd_led_enabled", "hdd_led_chip", "hdd_led_offset",
        "hdd_led_active_low", "power_short_ms", "power_long_ms", "reset_pulse_ms",
    };
    int64_t power_short = 0;
    int64_t power_long = 0;
    int64_t reset_pulse = 0;
    bool ok = reject_unknown_fields(object, allowed, sizeof(allowed) / sizeof(allowed[0]),
                                    "GPIO ATX settings", error) &&
              get_bool(object, "use_default", &settings->use_default, error) &&
              get_bool(object, "power_button_enabled", &settings->power_button_enabled, error) &&
              get_string(object, "power_button_chip", settings->power_button_chip,
                         sizeof(settings->power_button_chip), error) &&
              get_integer(object, "power_button_offset", INT64_MIN, INT64_MAX,
                          &settings->power_button_offset, error) &&
              get_bool(object, "power_button_active_low", &settings->power_button_active_low,
                       error) &&
              get_bool(object, "power_button_open_drain", &settings->power_button_open_drain,
                       error) &&
              get_bool(object, "reset_button_enabled", &settings->reset_button_enabled, error) &&
              get_string(object, "reset_button_chip", settings->reset_button_chip,
                         sizeof(settings->reset_button_chip), error) &&
              get_integer(object, "reset_button_offset", INT64_MIN, INT64_MAX,
                          &settings->reset_button_offset, error) &&
              get_bool(object, "reset_button_active_low", &settings->reset_button_active_low,
                       error) &&
              get_bool(object, "reset_button_open_drain", &settings->reset_button_open_drain,
                       error) &&
              get_bool(object, "pwr_led_enabled", &settings->pwr_led_enabled, error) &&
              get_string(object, "pwr_led_chip", settings->pwr_led_chip,
                         sizeof(settings->pwr_led_chip), error) &&
              get_integer(object, "pwr_led_offset", INT64_MIN, INT64_MAX,
                          &settings->pwr_led_offset, error) &&
              get_bool(object, "pwr_led_active_low", &settings->pwr_led_active_low, error) &&
              get_bool(object, "hdd_led_enabled", &settings->hdd_led_enabled, error) &&
              get_string(object, "hdd_led_chip", settings->hdd_led_chip,
                         sizeof(settings->hdd_led_chip), error) &&
              get_integer(object, "hdd_led_offset", INT64_MIN, INT64_MAX,
                          &settings->hdd_led_offset, error) &&
              get_bool(object, "hdd_led_active_low", &settings->hdd_led_active_low, error) &&
              get_integer(object, "power_short_ms", INT_MIN, INT_MAX, &power_short, error) &&
              get_integer(object, "power_long_ms", INT_MIN, INT_MAX, &power_long, error) &&
              get_integer(object, "reset_pulse_ms", INT_MIN, INT_MAX, &reset_pulse, error);
    json_object_put(object);
    if (!ok) return false;
    settings->power_short_ms = (int)power_short;
    settings->power_long_ms = (int)power_long;
    settings->reset_pulse_ms = (int)reset_pulse;
    return true;
}

static bool validate_line(bool enabled, const char *chip, int64_t offset, bool active_low,
                          bool open_drain, const char *name, LineConfig *line, char *error) {
    memset(line, 0, sizeof(*line));
    if (offset < 0 || (uint64_t)offset > UINT32_MAX) {
        set_error(error, "%s_offset must be between 0 and 4294967295", name);
        return false;
    }
    if (!enabled) return true;
    if (chip[0] == '\0') {
        set_error(error, "%s: enabled GPIO line requires a chip path", name);
        return false;
    }
    if (!copy_string(line->chip, sizeof(line->chip), chip, name, error)) return false;
    line->present = true;
    line->offset = (uint32_t)offset;
    line->active_low = active_low;
    line->open_drain = open_drain;
    return true;
}

static bool validate_duration(int value, const char *name, char *error) {
    if (value < 1 || value > MAXIMUM_PULSE_MS) {
        set_error(error, "%s must be between 1 and 30000", name);
        return false;
    }
    return true;
}

static bool config_from_settings(const Settings *settings, ATXConfig *config, char *error) {
    memset(config, 0, sizeof(*config));
    if (!validate_line(settings->power_button_enabled, settings->power_button_chip,
                       settings->power_button_offset, settings->power_button_active_low,
                       settings->power_button_open_drain, "power_button", &config->power_button,
                       error) ||
        !validate_line(settings->reset_button_enabled, settings->reset_button_chip,
                       settings->reset_button_offset, settings->reset_button_active_low,
                       settings->reset_button_open_drain, "reset_button", &config->reset_button,
                       error) ||
        !validate_line(settings->pwr_led_enabled, settings->pwr_led_chip,
                       settings->pwr_led_offset, settings->pwr_led_active_low, false, "pwr_led",
                       &config->pwr_led, error) ||
        !validate_line(settings->hdd_led_enabled, settings->hdd_led_chip,
                       settings->hdd_led_offset, settings->hdd_led_active_low, false, "hdd_led",
                       &config->hdd_led, error) ||
        !validate_duration(settings->power_short_ms, "power_short_ms", error) ||
        !validate_duration(settings->power_long_ms, "power_long_ms", error) ||
        !validate_duration(settings->reset_pulse_ms, "reset_pulse_ms", error)) {
        return false;
    }
    config->power_short_ms = settings->power_short_ms;
    config->power_long_ms = settings->power_long_ms;
    config->reset_pulse_ms = settings->reset_pulse_ms;
    return true;
}

static void copy_line_to_settings(const LineConfig *line, bool *enabled, char *chip, size_t size,
                                  int64_t *offset, bool *active_low, bool *open_drain) {
    if (!line->present) return;
    *enabled = true;
    snprintf(chip, size, "%s", line->chip);
    *offset = line->offset;
    *active_low = line->active_low;
    if (open_drain != NULL) *open_drain = line->open_drain;
}

static Settings settings_from_config(const ATXConfig *config, bool use_default) {
    Settings settings = {0};
    settings.use_default = use_default;
    settings.power_short_ms = config->power_short_ms == 0 ? DEFAULT_POWER_SHORT_MS
                                                          : config->power_short_ms;
    settings.power_long_ms = config->power_long_ms == 0 ? DEFAULT_POWER_LONG_MS
                                                        : config->power_long_ms;
    settings.reset_pulse_ms = config->reset_pulse_ms == 0 ? DEFAULT_RESET_PULSE_MS
                                                          : config->reset_pulse_ms;
    copy_line_to_settings(&config->power_button, &settings.power_button_enabled,
                          settings.power_button_chip, sizeof(settings.power_button_chip),
                          &settings.power_button_offset, &settings.power_button_active_low,
                          &settings.power_button_open_drain);
    copy_line_to_settings(&config->reset_button, &settings.reset_button_enabled,
                          settings.reset_button_chip, sizeof(settings.reset_button_chip),
                          &settings.reset_button_offset, &settings.reset_button_active_low,
                          &settings.reset_button_open_drain);
    copy_line_to_settings(&config->pwr_led, &settings.pwr_led_enabled, settings.pwr_led_chip,
                          sizeof(settings.pwr_led_chip), &settings.pwr_led_offset,
                          &settings.pwr_led_active_low, NULL);
    copy_line_to_settings(&config->hdd_led, &settings.hdd_led_enabled, settings.hdd_led_chip,
                          sizeof(settings.hdd_led_chip), &settings.hdd_led_offset,
                          &settings.hdd_led_active_low, NULL);
    return settings;
}

static json_object *settings_json(const Settings *settings) {
    json_object *object = json_object_new_object();
    if (object == NULL) return NULL;
#define ADD_BOOL(name) json_object_object_add(object, #name, json_object_new_boolean(settings->name))
#define ADD_STRING(name) json_object_object_add(object, #name, json_object_new_string(settings->name))
#define ADD_INTEGER(name) json_object_object_add(object, #name, json_object_new_int64(settings->name))
    ADD_BOOL(use_default);
    ADD_BOOL(power_button_enabled);
    ADD_STRING(power_button_chip);
    ADD_INTEGER(power_button_offset);
    ADD_BOOL(power_button_active_low);
    ADD_BOOL(power_button_open_drain);
    ADD_BOOL(reset_button_enabled);
    ADD_STRING(reset_button_chip);
    ADD_INTEGER(reset_button_offset);
    ADD_BOOL(reset_button_active_low);
    ADD_BOOL(reset_button_open_drain);
    ADD_BOOL(pwr_led_enabled);
    ADD_STRING(pwr_led_chip);
    ADD_INTEGER(pwr_led_offset);
    ADD_BOOL(pwr_led_active_low);
    ADD_BOOL(hdd_led_enabled);
    ADD_STRING(hdd_led_chip);
    ADD_INTEGER(hdd_led_offset);
    ADD_BOOL(hdd_led_active_low);
    ADD_INTEGER(power_short_ms);
    ADD_INTEGER(power_long_ms);
    ADD_INTEGER(reset_pulse_ms);
#undef ADD_BOOL
#undef ADD_STRING
#undef ADD_INTEGER
    return object;
}

static json_object *state_json(const ATXState *state) {
    json_object *object = json_object_new_object();
    if (object == NULL) return NULL;
    if (state->pwr_led_present) {
        json_object_object_add(object, "pwr_led", json_object_new_boolean(state->pwr_led));
    }
    if (state->hdd_led_present) {
        json_object_object_add(object, "hdd_led", json_object_new_boolean(state->hdd_led));
    }
    return object;
}

static void gpio_line_init(GPIOLine *line) {
    line->fd = -1;
}

static void gpio_line_close(GPIOLine *line) {
    if (line->fd >= 0) close(line->fd);
    line->fd = -1;
}

static bool gpio_line_open(GPIOLine *line, const LineConfig *config, bool output, char *error) {
    int chip_fd = open(config->chip, O_RDONLY | O_CLOEXEC);
    if (chip_fd < 0) {
        set_error(error, "open %s: %s", config->chip, strerror(errno));
        return false;
    }
    struct gpio_v2_line_request request = {0};
    request.offsets[0] = config->offset;
    snprintf(request.consumer, sizeof(request.consumer), "%s", "onekvm-gpio-atx");
    request.num_lines = 1;
    if (output) {
        request.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
        if (config->open_drain) request.config.flags |= GPIO_V2_LINE_FLAG_OPEN_DRAIN;
        request.config.num_attrs = 1;
        request.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
        request.config.attrs[0].attr.values = 0;
        request.config.attrs[0].mask = 1;
    } else {
        request.config.flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_RISING |
                               GPIO_V2_LINE_FLAG_EDGE_FALLING;
        request.event_buffer_size = 16;
    }
    if (config->active_low) request.config.flags |= GPIO_V2_LINE_FLAG_ACTIVE_LOW;
    if (ioctl(chip_fd, (int)GPIO_V2_GET_LINE_IOCTL, &request) < 0) {
        int saved = errno;
        close(chip_fd);
        set_error(error, "request GPIO line: %s", strerror(saved));
        return false;
    }
    close(chip_fd);
    if (request.fd < 0) {
        set_error(error, "kernel returned invalid GPIO line descriptor");
        return false;
    }
    line->fd = request.fd;
    int descriptor_flags = fcntl(line->fd, F_GETFD);
    if (descriptor_flags >= 0) (void)fcntl(line->fd, F_SETFD, descriptor_flags | FD_CLOEXEC);
    if (!output) {
        int status_flags = fcntl(line->fd, F_GETFL);
        if (status_flags >= 0) (void)fcntl(line->fd, F_SETFL, status_flags | O_NONBLOCK);
    }
    return true;
}

static bool gpio_line_get(const GPIOLine *line, bool *active, char *error) {
    struct gpio_v2_line_values values = {.mask = 1};
    if (ioctl(line->fd, (int)GPIO_V2_LINE_GET_VALUES_IOCTL, &values) < 0) {
        set_error(error, "read GPIO line: %s", strerror(errno));
        return false;
    }
    *active = (values.bits & 1U) != 0;
    return true;
}

static bool gpio_line_set(const GPIOLine *line, bool active, char *error) {
    struct gpio_v2_line_values values = {.bits = active ? 1U : 0U, .mask = 1};
    if (ioctl(line->fd, (int)GPIO_V2_LINE_SET_VALUES_IOCTL, &values) < 0) {
        set_error(error, "write GPIO line: %s", strerror(errno));
        return false;
    }
    return true;
}

static void gpio_line_drain(const GPIOLine *line) {
    struct gpio_v2_line_event event;
    for (;;) {
        ssize_t count = read(line->fd, &event, sizeof(event));
        if (count == (ssize_t)sizeof(event)) continue;
        if (count < 0 && errno == EINTR) continue;
        break;
    }
}

static void controller_init(Controller *controller) {
    memset(controller, 0, sizeof(*controller));
    gpio_line_init(&controller->power);
    gpio_line_init(&controller->reset);
    gpio_line_init(&controller->pwr_led);
    gpio_line_init(&controller->hdd_led);
}

static void controller_close(Controller *controller) {
    gpio_line_close(&controller->power);
    gpio_line_close(&controller->reset);
    gpio_line_close(&controller->pwr_led);
    gpio_line_close(&controller->hdd_led);
    controller->last_state_set = false;
}

static bool controller_open_optional(GPIOLine *line, const LineConfig *config, bool output,
                                     const char *name, char *error) {
    if (!config->present) return true;
    if (config->chip[0] == '\0') {
        set_error(error, "%s GPIO chip is empty", name);
        return false;
    }
    if (!gpio_line_open(line, config, output, error)) {
        char previous[ERROR_SIZE];
        snprintf(previous, sizeof(previous), "%s", error);
        set_error(error, "open %s GPIO %s:%u: %s", name, config->chip, config->offset, previous);
        return false;
    }
    return true;
}

static bool controller_state(const Controller *controller, ATXState *state, char *error) {
    memset(state, 0, sizeof(*state));
    if (controller->pwr_led.fd >= 0) {
        state->pwr_led_present = true;
        if (!gpio_line_get(&controller->pwr_led, &state->pwr_led, error)) {
            prefix_error(error, "read PWR LED: ");
            return false;
        }
    }
    if (controller->hdd_led.fd >= 0) {
        state->hdd_led_present = true;
        if (!gpio_line_get(&controller->hdd_led, &state->hdd_led, error)) {
            prefix_error(error, "read HDD LED: ");
            return false;
        }
    }
    return true;
}

static bool state_equal(const ATXState *left, const ATXState *right) {
    return left->pwr_led_present == right->pwr_led_present &&
           (!left->pwr_led_present || left->pwr_led == right->pwr_led) &&
           left->hdd_led_present == right->hdd_led_present &&
           (!left->hdd_led_present || left->hdd_led == right->hdd_led);
}

static bool controller_configure(Controller *controller, ATXConfig config, char *error) {
    controller_init(controller);
    if (config.power_short_ms == 0) config.power_short_ms = DEFAULT_POWER_SHORT_MS;
    if (config.power_long_ms == 0) config.power_long_ms = DEFAULT_POWER_LONG_MS;
    if (config.reset_pulse_ms == 0) config.reset_pulse_ms = DEFAULT_RESET_PULSE_MS;
    if (!validate_duration(config.power_short_ms, "power_short_ms", error) ||
        !validate_duration(config.power_long_ms, "power_long_ms", error) ||
        !validate_duration(config.reset_pulse_ms, "reset_pulse_ms", error) ||
        !controller_open_optional(&controller->power, &config.power_button, true, "power button",
                                  error) ||
        !controller_open_optional(&controller->reset, &config.reset_button, true, "reset button",
                                  error) ||
        !controller_open_optional(&controller->pwr_led, &config.pwr_led, false, "PWR LED", error) ||
        !controller_open_optional(&controller->hdd_led, &config.hdd_led, false, "HDD LED", error)) {
        controller_close(controller);
        return false;
    }
    controller->config = config;
    if (!controller_state(controller, &controller->last_state, error)) {
        controller_close(controller);
        return false;
    }
    controller->last_state_set = true;
    return true;
}

static bool controller_action(Controller *controller, const char *action, char *error) {
    GPIOLine *line = NULL;
    int duration_ms = 0;
    if (strcmp(action, "power-short") == 0) {
        line = controller->power.fd >= 0 ? &controller->power : NULL;
        duration_ms = controller->config.power_short_ms;
    } else if (strcmp(action, "power-long") == 0) {
        line = controller->power.fd >= 0 ? &controller->power : NULL;
        duration_ms = controller->config.power_long_ms;
    } else if (strcmp(action, "reset") == 0) {
        line = controller->reset.fd >= 0 ? &controller->reset : NULL;
        duration_ms = controller->config.reset_pulse_ms;
    } else {
        set_error(error, "unsupported ATX action \"%s\"", action);
        return false;
    }
    if (line == NULL) {
        set_error(error, "ATX action \"%s\" is not configured", action);
        return false;
    }
    if (!gpio_line_set(line, true, error)) return false;
    struct timespec remaining = {
        .tv_sec = duration_ms / 1000,
        .tv_nsec = (long)(duration_ms % 1000) * 1000000L,
    };
    while (nanosleep(&remaining, &remaining) < 0 && errno == EINTR) {
    }
    if (!gpio_line_set(line, false, error)) {
        prefix_error(error, "release ATX line: ");
        return false;
    }
    return true;
}

static bool controller_changed_state(Controller *controller, ATXState *state, bool *changed,
                                     char *error) {
    if (!controller_state(controller, state, error)) return false;
    *changed = !controller->last_state_set || !state_equal(state, &controller->last_state);
    if (*changed) {
        controller->last_state = *state;
        controller->last_state_set = true;
    }
    return true;
}

static bool read_atx_config_file(const char *path, ATXConfig *config, char *error) {
    char *payload = NULL;
    if (!read_file(path, &payload, NULL, error)) return false;
    bool ok = parse_atx_config(payload, config, error);
    free(payload);
    return ok;
}

static bool service_replace_controller(Service *service, const ATXConfig *config, char *error) {
    Controller next;
    if (!controller_configure(&next, *config, error)) {
        prefix_error(error, "initialize GPIO ATX: ");
        return false;
    }
    if (service->controller_valid) controller_close(&service->controller);
    service->controller = next;
    service->controller_valid = true;
    return true;
}

static bool service_initialize(Service *service, char *error) {
    memset(service, 0, sizeof(*service));
    controller_init(&service->controller);
    Settings saved;
    char *payload = NULL;
    int open_error = 0;
    if (read_file(USER_CONFIG_PATH, &payload, &open_error, error)) {
        bool parsed = parse_settings(payload, &saved, error);
        free(payload);
        if (!parsed) {
            prefix_error(error, "read GPIO ATX user settings: ");
            return false;
        }
        if (!saved.use_default) {
            ATXConfig config;
            if (!config_from_settings(&saved, &config, error) ||
                !service_replace_controller(service, &config, error)) {
                return false;
            }
            service->settings = saved;
            return true;
        }
    } else if (open_error != ENOENT) {
        prefix_error(error, "read GPIO ATX user settings: ");
        return false;
    }
    ATXConfig config;
    if (!read_atx_config_file(DEFAULT_CONFIG_PATH, &config, error)) {
        prefix_error(error, "read machine GPIO ATX default: ");
        return false;
    }
    if (!service_replace_controller(service, &config, error)) return false;
    service->settings = settings_from_config(&config, true);
    return true;
}

static bool service_restore(Service *service, const ATXConfig *config, const Settings *settings,
                            const char *cause, char *error) {
    Controller rollback;
    char rollback_error[ERROR_SIZE] = {0};
    if (!controller_configure(&rollback, *config, rollback_error)) {
        set_error(error, "%s; restore previous GPIO ATX settings: %s", cause, rollback_error);
        service->controller_valid = false;
        return false;
    }
    service->controller = rollback;
    service->controller_valid = true;
    service->settings = *settings;
    set_error(error, "%s", cause);
    return false;
}

static bool service_update_settings(Service *service, const char *payload, char *error) {
    Settings requested;
    if (!parse_settings(payload, &requested, error)) {
        prefix_error(error, "decode GPIO ATX settings: ");
        return false;
    }
    Settings selected = requested;
    ATXConfig next_config;
    if (requested.use_default) {
        if (!read_atx_config_file(DEFAULT_CONFIG_PATH, &next_config, error)) return false;
        selected = settings_from_config(&next_config, true);
    } else if (!config_from_settings(&requested, &next_config, error)) {
        return false;
    }
    ATXConfig old_config;
    if (!config_from_settings(&service->settings, &old_config, error)) {
        prefix_error(error, "preserve current GPIO ATX settings: ");
        return false;
    }
    Settings old_settings = service->settings;
    if (service->controller_valid) controller_close(&service->controller);
    service->controller_valid = false;

    Controller next;
    if (!controller_configure(&next, next_config, error)) {
        char cause[ERROR_SIZE];
        snprintf(cause, sizeof(cause), "apply GPIO ATX settings: %s", error);
        return service_restore(service, &old_config, &old_settings, cause, error);
    }
    json_object *object = settings_json(&requested);
    if (object == NULL) {
        controller_close(&next);
        return service_restore(service, &old_config, &old_settings,
                               "encode GPIO ATX settings: out of memory", error);
    }
    const char *encoded = json_object_to_json_string_ext(object, JSON_C_TO_STRING_PRETTY);
    bool persisted = encoded != NULL && atomic_write(USER_CONFIG_PATH, encoded, error);
    char write_error[ERROR_SIZE];
    snprintf(write_error, sizeof(write_error), "%s",
             encoded == NULL ? "encode GPIO ATX settings: out of memory" : error);
    json_object_put(object);
    if (!persisted) {
        controller_close(&next);
        return service_restore(service, &old_config, &old_settings, write_error, error);
    }
    service->controller = next;
    service->controller_valid = true;
    service->settings = selected;
    return true;
}

static int dbus_failure(sd_bus_error *return_error, const char *message) {
    return sd_bus_error_setf(return_error, SD_BUS_ERROR_FAILED, "%s", message);
}

static int method_get_capabilities(sd_bus_message *message, void *userdata,
                                   sd_bus_error *return_error) {
    (void)userdata;
    (void)return_error;
    return sd_bus_reply_method_return(message, "s",
                                      "{\"api_major\":1,\"api_minor\":2,\"atx\":true}");
}

static int method_configure_atx(sd_bus_message *message, void *userdata,
                                sd_bus_error *return_error) {
    (void)userdata;
    const char *payload = NULL;
    int result = sd_bus_message_read(message, "s", &payload);
    if (result < 0) return result;
    ATXConfig ignored;
    char error[ERROR_SIZE] = {0};
    if (!parse_atx_config(payload == NULL ? "" : payload, &ignored, error)) {
        prefix_error(error, "decode ATX fallback: ");
        return dbus_failure(return_error, error);
    }
    return sd_bus_reply_method_return(message, "");
}

static int method_atx_action(sd_bus_message *message, void *userdata,
                             sd_bus_error *return_error) {
    const char *action = NULL;
    int result = sd_bus_message_read(message, "s", &action);
    if (result < 0) return result;
    Service *service = userdata;
    char error[ERROR_SIZE] = {0};
    if (!service->controller_valid ||
        !controller_action(&service->controller, action == NULL ? "" : action, error)) {
        if (error[0] == '\0') snprintf(error, sizeof(error), "%s", "ATX is not configured");
        return dbus_failure(return_error, error);
    }
    return sd_bus_reply_method_return(message, "");
}

static int method_get_atx_state(sd_bus_message *message, void *userdata,
                                sd_bus_error *return_error) {
    Service *service = userdata;
    if (!service->controller_valid) return dbus_failure(return_error, "ATX is not configured");
    ATXState state;
    char error[ERROR_SIZE] = {0};
    if (!controller_state(&service->controller, &state, error)) {
        return dbus_failure(return_error, error);
    }
    json_object *object = state_json(&state);
    if (object == NULL) return dbus_failure(return_error, "encode ATX state: out of memory");
    const char *payload = json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN);
    int result = sd_bus_reply_method_return(message, "s", payload == NULL ? "{}" : payload);
    json_object_put(object);
    return result;
}

static int method_get_settings(sd_bus_message *message, void *userdata,
                               sd_bus_error *return_error) {
    Service *service = userdata;
    json_object *object = settings_json(&service->settings);
    if (object == NULL) return dbus_failure(return_error, "encode GPIO ATX settings: out of memory");
    const char *payload = json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN);
    int result = sd_bus_reply_method_return(message, "s", payload == NULL ? "{}" : payload);
    json_object_put(object);
    return result;
}

static int method_update_settings(sd_bus_message *message, void *userdata,
                                  sd_bus_error *return_error) {
    const char *payload = NULL;
    int result = sd_bus_message_read(message, "s", &payload);
    if (result < 0) return result;
    char error[ERROR_SIZE] = {0};
    if (!service_update_settings(userdata, payload == NULL ? "" : payload, error)) {
        return dbus_failure(return_error, error);
    }
    return sd_bus_reply_method_return(message, "");
}

static const sd_bus_vtable vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("GetCapabilities", "", "s", method_get_capabilities,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ConfigureATX", "s", "", method_configure_atx, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ATXAction", "s", "", method_atx_action, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetATXState", "", "s", method_get_atx_state, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetSettings", "", "s", method_get_settings, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("UpdateSettings", "s", "", method_update_settings, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("ATXStateChanged", "s", 0),
    SD_BUS_VTABLE_END,
};

static int poll_timeout_ms(sd_bus *bus) {
    uint64_t timeout_usec = UINT64_MAX;
    if (sd_bus_get_timeout(bus, &timeout_usec) < 0 || timeout_usec == UINT64_MAX) return -1;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
    uint64_t now_usec = (uint64_t)now.tv_sec * 1000000ULL + (uint64_t)now.tv_nsec / 1000ULL;
    if (timeout_usec <= now_usec) return 0;
    uint64_t wait_ms = (timeout_usec - now_usec + 999ULL) / 1000ULL;
    return wait_ms > INT_MAX ? INT_MAX : (int)wait_ms;
}

static uint64_t monotonic_usec(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0;
    return (uint64_t)now.tv_sec * 1000000ULL + (uint64_t)now.tv_nsec / 1000ULL;
}

static bool arm_led_timer(int timer_fd, uint64_t delay_usec) {
    struct itimerspec timer = {0};
    timer.it_value.tv_sec = (time_t)(delay_usec / 1000000ULL);
    timer.it_value.tv_nsec = (long)(delay_usec % 1000000ULL) * 1000L;
    if (timer.it_value.tv_sec == 0 && timer.it_value.tv_nsec == 0) timer.it_value.tv_nsec = 1;
    return timerfd_settime(timer_fd, 0, &timer, NULL) == 0;
}

static void publish_led_state(sd_bus *bus, Controller *controller,
                              uint64_t *last_publish_usec) {
    ATXState state;
    bool changed = false;
    char error[ERROR_SIZE] = {0};
    if (!controller_changed_state(controller, &state, &changed, error) || !changed) return;
    json_object *object = state_json(&state);
    if (object == NULL) return;
    const char *payload = json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN);
    int result = sd_bus_emit_signal(bus, OBJECT_PATH, INTERFACE, "ATXStateChanged", "s",
                                    payload == NULL ? "{}" : payload);
    json_object_put(object);
    if (result < 0) {
        fprintf(stderr, "onekvm-device-gpio-atx: emit ATX state: %s\n", strerror(-result));
        return;
    }
    *last_publish_usec = monotonic_usec();
}

static int run(void) {
    sigset_t signal_mask;
    sigemptyset(&signal_mask);
    sigaddset(&signal_mask, SIGINT);
    sigaddset(&signal_mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &signal_mask, NULL) < 0) {
        fprintf(stderr, "onekvm-device-gpio-atx: block termination signals: %s\n",
                strerror(errno));
        return 1;
    }
    int signal_fd = signalfd(-1, &signal_mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (signal_fd < 0) {
        fprintf(stderr, "onekvm-device-gpio-atx: create signal descriptor: %s\n",
                strerror(errno));
        return 1;
    }
    int led_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (led_timer_fd < 0) {
        fprintf(stderr, "onekvm-device-gpio-atx: create LED timer: %s\n", strerror(errno));
        close(signal_fd);
        return 1;
    }

    Service service;
    char error[ERROR_SIZE] = {0};
    if (!service_initialize(&service, error)) {
        fprintf(stderr, "onekvm-device-gpio-atx: %s\n", error);
        close(led_timer_fd);
        close(signal_fd);
        return 1;
    }

    sd_bus *bus = NULL;
    sd_bus_slot *slot = NULL;
    int result = sd_bus_default_system(&bus);
    if (result < 0) {
        fprintf(stderr, "onekvm-device-gpio-atx: connect system D-Bus: %s\n", strerror(-result));
        controller_close(&service.controller);
        close(led_timer_fd);
        close(signal_fd);
        return 1;
    }
    result = sd_bus_add_object_vtable(bus, &slot, OBJECT_PATH, INTERFACE, vtable, &service);
    if (result < 0) {
        fprintf(stderr, "onekvm-device-gpio-atx: export GPIO ATX service: %s\n",
                strerror(-result));
        sd_bus_unref(bus);
        controller_close(&service.controller);
        close(led_timer_fd);
        close(signal_fd);
        return 1;
    }
    result = sd_bus_request_name(bus, BUS_NAME, 0);
    if (result < 0) {
        fprintf(stderr, "onekvm-device-gpio-atx: request D-Bus name %s: %s\n", BUS_NAME,
                strerror(-result));
        sd_bus_slot_unref(slot);
        sd_bus_unref(bus);
        controller_close(&service.controller);
        close(led_timer_fd);
        close(signal_fd);
        return 1;
    }

    bool stopping = false;
    bool failed = false;
    bool led_publish_pending = false;
    uint64_t last_led_publish_usec = 0;
    while (!stopping) {
        do {
            result = sd_bus_process(bus, NULL);
            if (result < 0) {
                fprintf(stderr, "onekvm-device-gpio-atx: process D-Bus: %s\n",
                        strerror(-result));
                failed = true;
                stopping = true;
                break;
            }
        } while (result > 0);
        if (stopping) break;

        struct pollfd descriptors[5] = {
            {.fd = sd_bus_get_fd(bus), .events = (short)sd_bus_get_events(bus)},
            {.fd = signal_fd, .events = POLLIN},
            {.fd = service.controller_valid ? service.controller.pwr_led.fd : -1,
             .events = POLLIN},
            {.fd = service.controller_valid ? service.controller.hdd_led.fd : -1,
             .events = POLLIN},
            {.fd = led_timer_fd, .events = POLLIN},
        };
        int ready = poll(descriptors, sizeof(descriptors) / sizeof(descriptors[0]),
                         poll_timeout_ms(bus));
        if (ready < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "onekvm-device-gpio-atx: poll: %s\n", strerror(errno));
            failed = true;
            break;
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            struct signalfd_siginfo info;
            (void)read(signal_fd, &info, sizeof(info));
            stopping = true;
            continue;
        }
        bool pwr_ready = (descriptors[2].revents & POLLIN) != 0;
        bool hdd_ready = (descriptors[3].revents & POLLIN) != 0;
        if ((pwr_ready || hdd_ready) && service.controller_valid) {
            if (pwr_ready && service.controller.pwr_led.fd >= 0) {
                gpio_line_drain(&service.controller.pwr_led);
            }
            if (hdd_ready && service.controller.hdd_led.fd >= 0) {
                gpio_line_drain(&service.controller.hdd_led);
            }
            uint64_t now = monotonic_usec();
            if (last_led_publish_usec == 0 ||
                now >= last_led_publish_usec + LED_PUBLISH_INTERVAL_USEC) {
                publish_led_state(bus, &service.controller, &last_led_publish_usec);
                led_publish_pending = false;
            } else {
                led_publish_pending = true;
                (void)arm_led_timer(led_timer_fd,
                                    last_led_publish_usec + LED_PUBLISH_INTERVAL_USEC - now);
            }
        }
        if ((descriptors[4].revents & POLLIN) != 0) {
            uint64_t expirations;
            (void)read(led_timer_fd, &expirations, sizeof(expirations));
            if (led_publish_pending && service.controller_valid) {
                publish_led_state(bus, &service.controller, &last_led_publish_usec);
                led_publish_pending = false;
            }
        }
    }

    (void)sd_bus_release_name(bus, BUS_NAME);
    sd_bus_slot_unref(slot);
    sd_bus_flush_close_unref(bus);
    if (service.controller_valid) controller_close(&service.controller);
    close(led_timer_fd);
    close(signal_fd);
    return failed ? 1 : 0;
}

int main(void) {
    return run();
}
