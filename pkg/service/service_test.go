package service

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"testing"

	"github.com/onekvm/onekvm-plugins-gpio/pkg/contract"
)

type fakeController struct {
	configured contract.ATXConfig
	changes    chan contract.ATXState
	closed     bool
	configure  error
}

func (c *fakeController) Configure(config contract.ATXConfig) error {
	c.configured = config
	return c.configure
}

func TestRejectedUserOverrideRestoresWorkingConfiguration(t *testing.T) {
	root := t.TempDir()
	defaultPath := filepath.Join(root, "run", "default.json")
	userPath := filepath.Join(root, "etc", "gpio-atx.json")
	if err := os.MkdirAll(filepath.Dir(defaultPath), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(defaultPath, []byte(`{
  "power_button":{"chip":"/dev/gpiochip4","offset":23},
  "power_short_ms":200,"power_long_ms":5000,"reset_pulse_ms":200
}`), 0o644); err != nil {
		t.Fatal(err)
	}
	var controllers []*fakeController
	factory := func() controller {
		current := &fakeController{changes: make(chan contract.ATXState)}
		if len(controllers) == 1 {
			current.configure = errors.New("GPIO line is busy")
		}
		controllers = append(controllers, current)
		return current
	}
	provider, err := newService(defaultPath, userPath, factory)
	if err != nil {
		t.Fatal(err)
	}
	defer provider.Close()

	payload, dbusErr := provider.GetSettings()
	if dbusErr != nil {
		t.Fatal(dbusErr)
	}
	var settings Settings
	if err := json.Unmarshal([]byte(payload), &settings); err != nil {
		t.Fatal(err)
	}
	settings.UseDefault = false
	settings.PowerButtonOffset = 99
	update, _ := json.Marshal(settings)
	if dbusErr := provider.UpdateSettings(string(update)); dbusErr == nil {
		t.Fatal("invalid GPIO override unexpectedly succeeded")
	}
	if len(controllers) != 3 || !controllers[0].closed || !controllers[1].closed {
		t.Fatalf("controller rollback sequence = %+v", controllers)
	}
	if controllers[2].configured.PowerButton == nil || controllers[2].configured.PowerButton.Offset != 23 {
		t.Fatalf("restored configuration = %+v", controllers[2].configured)
	}
	if _, err := os.Stat(userPath); !os.IsNotExist(err) {
		t.Fatalf("rejected settings were persisted: %v", err)
	}
}
func (*fakeController) Action(string) error                 { return nil }
func (*fakeController) State() (contract.ATXState, error)   { return contract.ATXState{}, nil }
func (c *fakeController) Changes() <-chan contract.ATXState { return c.changes }
func (c *fakeController) Close() error                      { c.closed = true; return nil }

func TestMachineDefaultAndPersistentUserOverride(t *testing.T) {
	root := t.TempDir()
	defaultPath := filepath.Join(root, "run", "default.json")
	userPath := filepath.Join(root, "etc", "gpio-atx.json")
	if err := os.MkdirAll(filepath.Dir(defaultPath), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(defaultPath, []byte(`{
  "power_button":{"chip":"/dev/gpiochip4","offset":23},
  "pwr_led":{"chip":"/dev/gpiochip4","offset":24,"active_low":true},
  "power_short_ms":200,"power_long_ms":5000,"reset_pulse_ms":200
}`), 0o644); err != nil {
		t.Fatal(err)
	}
	var controllers []*fakeController
	factory := func() controller {
		current := &fakeController{changes: make(chan contract.ATXState)}
		controllers = append(controllers, current)
		return current
	}
	provider, err := newService(defaultPath, userPath, factory)
	if err != nil {
		t.Fatal(err)
	}
	defer provider.Close()
	if len(controllers) != 1 || controllers[0].configured.PowerButton == nil || controllers[0].configured.PowerButton.Offset != 23 {
		t.Fatalf("default configuration = %+v", controllers)
	}
	payload, dbusErr := provider.GetSettings()
	if dbusErr != nil {
		t.Fatal(dbusErr)
	}
	var settings Settings
	if err := json.Unmarshal([]byte(payload), &settings); err != nil {
		t.Fatal(err)
	}
	if !settings.UseDefault || settings.PowerButtonOffset != 23 || !settings.PWRLEDActiveLow {
		t.Fatalf("default settings = %+v", settings)
	}

	settings.UseDefault = false
	settings.PowerButtonOffset = 7
	update, _ := json.Marshal(settings)
	if dbusErr := provider.UpdateSettings(string(update)); dbusErr != nil {
		t.Fatal(dbusErr)
	}
	if len(controllers) != 2 || controllers[1].configured.PowerButton == nil || controllers[1].configured.PowerButton.Offset != 7 {
		t.Fatalf("custom configuration = %+v", controllers)
	}
	data, err := os.ReadFile(userPath)
	if err != nil {
		t.Fatal(err)
	}
	var saved Settings
	if err := json.Unmarshal(data, &saved); err != nil {
		t.Fatal(err)
	}
	if saved.UseDefault || saved.PowerButtonOffset != 7 {
		t.Fatalf("saved settings = %+v", saved)
	}
}
