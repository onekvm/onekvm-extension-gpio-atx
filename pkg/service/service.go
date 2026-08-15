package service

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sync"

	"github.com/godbus/dbus/v5"
	"github.com/onekvm/onekvm-plugins-gpio/pkg/contract"
	"github.com/onekvm/onekvm-plugins-gpio/pkg/gpioatx"
)

const (
	BusName           = "org.onekvm.Device.GPIO1"
	ObjectPath        = dbus.ObjectPath("/org/onekvm/Device1")
	DefaultConfigPath = "/run/onekvm/gpio-atx-default.json"
	UserConfigPath    = "/etc/onekvm/plugins/gpio-atx.json"
)

type controller interface {
	Configure(contract.ATXConfig) error
	Action(string) error
	State() (contract.ATXState, error)
	Changes() <-chan contract.ATXState
	Close() error
}

type Service struct {
	mu          sync.Mutex
	defaultPath string
	userPath    string
	newControl  func() controller
	control     controller
	settings    Settings
	changes     chan contract.ATXState
	closed      chan struct{}
	closeOnce   sync.Once
}

func New() (*Service, error) {
	return newService(DefaultConfigPath, UserConfigPath, func() controller { return gpioatx.NewController() })
}

func newService(defaultPath, userPath string, factory func() controller) (*Service, error) {
	s := &Service{
		defaultPath: defaultPath, userPath: userPath, newControl: factory,
		changes: make(chan contract.ATXState, 1), closed: make(chan struct{}),
	}
	settings, config, err := s.loadSelectedConfig()
	if err != nil {
		return nil, err
	}
	if err := s.replaceController(config); err != nil {
		return nil, err
	}
	s.settings = settings
	return s, nil
}

func (*Service) GetCapabilities() (string, *dbus.Error) {
	payload, err := json.Marshal(contract.Capabilities{APIMajor: contract.APIMajor, APIMinor: contract.APIMinor, ATX: true})
	if err != nil {
		return "", dbusError(err)
	}
	return string(payload), nil
}

// ConfigureATX remains for Device1 API compatibility. GPIO selection belongs
// to this plugin, so a Core-provided machine fallback does not replace the
// selected machine default or the user's persistent override.
func (s *Service) ConfigureATX(payload string) *dbus.Error {
	var fallback contract.ATXConfig
	if err := decodeStrict([]byte(payload), &fallback); err != nil {
		return dbusError(fmt.Errorf("decode ATX fallback: %w", err))
	}
	return nil
}

func (s *Service) ATXAction(action string) *dbus.Error {
	s.mu.Lock()
	control := s.control
	s.mu.Unlock()
	if control == nil {
		return dbusError(fmt.Errorf("ATX is not configured"))
	}
	return dbusError(control.Action(action))
}

func (s *Service) GetATXState() (string, *dbus.Error) {
	s.mu.Lock()
	control := s.control
	s.mu.Unlock()
	if control == nil {
		return "", dbusError(fmt.Errorf("ATX is not configured"))
	}
	state, err := control.State()
	if err != nil {
		return "", dbusError(err)
	}
	payload, err := json.Marshal(state)
	if err != nil {
		return "", dbusError(err)
	}
	return string(payload), nil
}

func (s *Service) GetSettings() (string, *dbus.Error) {
	s.mu.Lock()
	settings := s.settings
	s.mu.Unlock()
	payload, err := json.Marshal(settings)
	if err != nil {
		return "", dbusError(err)
	}
	return string(payload), nil
}

func (s *Service) UpdateSettings(payload string) *dbus.Error {
	var requested Settings
	if err := decodeStrict([]byte(payload), &requested); err != nil {
		return dbusError(fmt.Errorf("decode GPIO ATX settings: %w", err))
	}
	selected := requested
	var config contract.ATXConfig
	var err error
	if requested.UseDefault {
		config, err = readConfig(s.defaultPath)
		if err == nil {
			selected = settingsFromConfig(config, true)
		}
	} else {
		config, err = requested.config()
	}
	if err != nil {
		return dbusError(err)
	}

	s.mu.Lock()
	defer s.mu.Unlock()
	old := s.control
	s.control = nil
	if old != nil {
		_ = old.Close()
	}
	next := s.newControl()
	if err := next.Configure(config); err != nil {
		_ = next.Close()
		return dbusError(fmt.Errorf("apply GPIO ATX settings: %w", err))
	}
	if err := writeSettings(s.userPath, requested); err != nil {
		_ = next.Close()
		return dbusError(err)
	}
	s.control, s.settings = next, selected
	s.forwardChanges(next)
	return nil
}

func (s *Service) Changes() <-chan contract.ATXState { return s.changes }

func (s *Service) loadSelectedConfig() (Settings, contract.ATXConfig, error) {
	var saved Settings
	if err := readJSON(s.userPath, &saved); err == nil {
		if !saved.UseDefault {
			config, err := saved.config()
			return saved, config, err
		}
	} else if !os.IsNotExist(err) {
		return Settings{}, contract.ATXConfig{}, fmt.Errorf("read GPIO ATX user settings: %w", err)
	}
	config, err := readConfig(s.defaultPath)
	if err != nil {
		return Settings{}, config, fmt.Errorf("read machine GPIO ATX default: %w", err)
	}
	return settingsFromConfig(config, true), config, nil
}

func (s *Service) replaceController(config contract.ATXConfig) error {
	control := s.newControl()
	if err := control.Configure(config); err != nil {
		_ = control.Close()
		return fmt.Errorf("initialize GPIO ATX: %w", err)
	}
	s.control = control
	s.forwardChanges(control)
	return nil
}

func (s *Service) forwardChanges(control controller) {
	go func() {
		for {
			select {
			case <-s.closed:
				return
			case state, ok := <-control.Changes():
				if !ok {
					return
				}
				s.mu.Lock()
				current := s.control == control
				s.mu.Unlock()
				if !current {
					return
				}
				select {
				case s.changes <- state:
				default:
				}
			}
		}
	}()
}

func (s *Service) Close() error {
	var err error
	s.closeOnce.Do(func() {
		close(s.closed)
		s.mu.Lock()
		control := s.control
		s.control = nil
		s.mu.Unlock()
		if control != nil {
			err = control.Close()
		}
	})
	return err
}

func readConfig(path string) (contract.ATXConfig, error) {
	var config contract.ATXConfig
	if err := readJSON(path, &config); err != nil {
		return config, err
	}
	return config, nil
}

func readJSON(path string, destination any) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	return decodeStrict(data, destination)
}

func decodeStrict(data []byte, destination any) error {
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(destination); err != nil {
		return err
	}
	var trailing any
	if err := decoder.Decode(&trailing); err != io.EOF {
		return fmt.Errorf("configuration contains trailing JSON")
	}
	return nil
}

func writeSettings(path string, settings Settings) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return fmt.Errorf("create GPIO ATX settings directory: %w", err)
	}
	payload, err := json.MarshalIndent(settings, "", "  ")
	if err != nil {
		return err
	}
	payload = append(payload, '\n')
	temporary, err := os.CreateTemp(filepath.Dir(path), ".gpio-atx-*.json")
	if err != nil {
		return fmt.Errorf("create GPIO ATX settings: %w", err)
	}
	temporaryPath := temporary.Name()
	defer os.Remove(temporaryPath)
	if err = temporary.Chmod(0o600); err == nil {
		_, err = temporary.Write(payload)
	}
	if closeErr := temporary.Close(); err == nil {
		err = closeErr
	}
	if err != nil {
		return fmt.Errorf("write GPIO ATX settings: %w", err)
	}
	if err := os.Rename(temporaryPath, path); err != nil {
		return fmt.Errorf("replace GPIO ATX settings: %w", err)
	}
	return nil
}

func dbusError(err error) *dbus.Error {
	if err == nil {
		return nil
	}
	return dbus.MakeFailedError(err)
}
