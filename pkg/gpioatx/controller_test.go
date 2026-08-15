package gpioatx

import (
	"reflect"
	"sync"
	"testing"
	"time"

	"github.com/onekvm/onekvm-plugins-gpio/pkg/contract"
)

type fakeLine struct {
	mu      sync.Mutex
	value   bool
	changes chan struct{}
	writes  []bool
}

func newFakeLine(value bool) *fakeLine {
	return &fakeLine{value: value, changes: make(chan struct{}, 4)}
}

func (l *fakeLine) Get() (bool, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.value, nil
}

func (l *fakeLine) Set(value bool) error {
	l.mu.Lock()
	l.value = value
	l.writes = append(l.writes, value)
	l.mu.Unlock()
	return nil
}

func TestControllerReportsOnlyConfiguredLEDLines(t *testing.T) {
	pwr := newFakeLine(true)
	controller := newController(func(config contract.GPIOLineConfig, output bool) (gpioLine, error) {
		if output {
			return newFakeLine(false), nil
		}
		return pwr, nil
	})
	defer controller.Close()
	if err := controller.Configure(contract.ATXConfig{
		PowerButton: &contract.GPIOLineConfig{Chip: "/dev/gpiochip0", Offset: 1},
		PWRLED:      &contract.GPIOLineConfig{Chip: "/dev/gpiochip0", Offset: 2},
	}); err != nil {
		t.Fatal(err)
	}
	state, err := controller.State()
	if err != nil {
		t.Fatal(err)
	}
	if state.PWRLED == nil || !*state.PWRLED || state.HDDLED != nil {
		t.Fatalf("state = %+v", state)
	}
}

func TestControllerPulsesAndReleasesButton(t *testing.T) {
	power := newFakeLine(false)
	controller := newController(func(contract.GPIOLineConfig, bool) (gpioLine, error) { return power, nil })
	defer controller.Close()
	if err := controller.Configure(contract.ATXConfig{
		PowerButton:  &contract.GPIOLineConfig{Chip: "/dev/gpiochip0", Offset: 1},
		PowerShortMS: 1,
	}); err != nil {
		t.Fatal(err)
	}
	if err := controller.Action(ActionPowerShort); err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(power.writes, []bool{true, false}) {
		t.Fatalf("writes = %#v", power.writes)
	}
}

func (l *fakeLine) Changes() <-chan struct{} { return l.changes }
func (l *fakeLine) Close() error             { return nil }

func (l *fakeLine) change(value bool) {
	l.mu.Lock()
	l.value = value
	l.mu.Unlock()
	l.changes <- struct{}{}
}

func TestControllerPublishesOnlyChangedLEDState(t *testing.T) {
	pwr := newFakeLine(false)
	controller := newController(func(config contract.GPIOLineConfig, output bool) (gpioLine, error) {
		if output {
			return newFakeLine(false), nil
		}
		return pwr, nil
	})
	defer controller.Close()
	if err := controller.Configure(contract.ATXConfig{
		PWRLED: &contract.GPIOLineConfig{Chip: "/dev/gpiochip0", Offset: 1},
	}); err != nil {
		t.Fatal(err)
	}

	pwr.change(false)
	select {
	case state := <-controller.Changes():
		t.Fatalf("unchanged edge emitted state %+v", state)
	case <-time.After(20 * time.Millisecond):
	}
	pwr.change(true)
	select {
	case state := <-controller.Changes():
		if state.PWRLED == nil || !*state.PWRLED {
			t.Fatalf("state = %+v", state)
		}
	case <-time.After(time.Second):
		t.Fatal("changed LED state was not published")
	}
}
