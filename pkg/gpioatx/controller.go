package gpioatx

import (
	"fmt"
	"sync"
	"time"

	"github.com/onekvm/onekvm-plugins-gpio/pkg/contract"
)

const (
	ActionPowerShort = "power-short"
	ActionPowerLong  = "power-long"
	ActionReset      = "reset"

	defaultShortPulse = 200 * time.Millisecond
	defaultLongPulse  = 5 * time.Second
)

type gpioLine interface {
	Get() (bool, error)
	Set(bool) error
	Changes() <-chan struct{}
	Close() error
}

type lineOpener func(contract.GPIOLineConfig, bool) (gpioLine, error)

type lines struct {
	power, reset, pwrLED, hddLED gpioLine
}

func (l lines) close() {
	for _, line := range []gpioLine{l.power, l.reset, l.pwrLED, l.hddLED} {
		if line != nil {
			_ = line.Close()
		}
	}
}

type Controller struct {
	mu       sync.RWMutex
	actionMu sync.Mutex
	openLine lineOpener
	lines    lines
	config   contract.ATXConfig
	changes  chan contract.ATXState
	stop     chan struct{}
	last     contract.ATXState
	lastSet  bool
}

func NewController() *Controller { return newController(openCDEVLine) }

func newController(opener lineOpener) *Controller {
	return &Controller{openLine: opener, changes: make(chan contract.ATXState, 1)}
}

func (c *Controller) Configure(config contract.ATXConfig) error {
	config.PowerShortMS = normalizedMilliseconds(config.PowerShortMS, defaultShortPulse)
	config.PowerLongMS = normalizedMilliseconds(config.PowerLongMS, defaultLongPulse)
	config.ResetPulseMS = normalizedMilliseconds(config.ResetPulseMS, defaultShortPulse)
	for name, value := range map[string]int{
		"power_short_ms": config.PowerShortMS,
		"power_long_ms":  config.PowerLongMS,
		"reset_pulse_ms": config.ResetPulseMS,
	} {
		if value < 1 || value > 30_000 {
			return fmt.Errorf("%s must be between 1 and 30000", name)
		}
	}

	var next lines
	open := func(name string, lineConfig *contract.GPIOLineConfig, output bool) (gpioLine, error) {
		if lineConfig == nil {
			return nil, nil
		}
		if lineConfig.Chip == "" {
			return nil, fmt.Errorf("%s GPIO chip is empty", name)
		}
		line, err := c.openLine(*lineConfig, output)
		if err != nil {
			return nil, fmt.Errorf("open %s GPIO %s:%d: %w", name, lineConfig.Chip, lineConfig.Offset, err)
		}
		return line, nil
	}
	var err error
	if next.power, err = open("power button", config.PowerButton, true); err != nil {
		return err
	}
	defer func() {
		if err != nil {
			next.close()
		}
	}()
	if next.reset, err = open("reset button", config.ResetButton, true); err != nil {
		return err
	}
	if next.pwrLED, err = open("PWR LED", config.PWRLED, false); err != nil {
		return err
	}
	if next.hddLED, err = open("HDD LED", config.HDDLED, false); err != nil {
		return err
	}

	c.actionMu.Lock()
	c.mu.Lock()
	old, oldStop := c.lines, c.stop
	c.lines, c.config = next, config
	c.stop = make(chan struct{})
	stop := c.stop
	c.lastSet = false
	c.mu.Unlock()
	c.actionMu.Unlock()
	if oldStop != nil {
		close(oldStop)
	}
	old.close()
	state, stateErr := c.State()
	if stateErr != nil {
		return stateErr
	}
	c.mu.Lock()
	c.last, c.lastSet = state, true
	c.mu.Unlock()
	for _, line := range []gpioLine{next.pwrLED, next.hddLED} {
		if line != nil {
			go c.watch(line, stop)
		}
	}
	return nil
}

func normalizedMilliseconds(value int, fallback time.Duration) int {
	if value == 0 {
		return int(fallback / time.Millisecond)
	}
	return value
}

func (c *Controller) Action(action string) error {
	c.actionMu.Lock()
	defer c.actionMu.Unlock()
	c.mu.RLock()
	defer c.mu.RUnlock()
	var line gpioLine
	var duration time.Duration
	switch action {
	case ActionPowerShort:
		line, duration = c.lines.power, time.Duration(c.config.PowerShortMS)*time.Millisecond
	case ActionPowerLong:
		line, duration = c.lines.power, time.Duration(c.config.PowerLongMS)*time.Millisecond
	case ActionReset:
		line, duration = c.lines.reset, time.Duration(c.config.ResetPulseMS)*time.Millisecond
	default:
		return fmt.Errorf("unsupported ATX action %q", action)
	}
	if line == nil {
		return fmt.Errorf("ATX action %q is not configured", action)
	}
	if err := line.Set(true); err != nil {
		return err
	}
	time.Sleep(duration)
	if err := line.Set(false); err != nil {
		return fmt.Errorf("release ATX line: %w", err)
	}
	return nil
}

func (c *Controller) State() (contract.ATXState, error) {
	c.mu.RLock()
	defer c.mu.RUnlock()
	return stateFromLines(c.lines)
}

func stateFromLines(value lines) (contract.ATXState, error) {
	var state contract.ATXState
	if value.pwrLED != nil {
		active, err := value.pwrLED.Get()
		if err != nil {
			return state, fmt.Errorf("read PWR LED: %w", err)
		}
		state.PWRLED = boolPointer(active)
	}
	if value.hddLED != nil {
		active, err := value.hddLED.Get()
		if err != nil {
			return state, fmt.Errorf("read HDD LED: %w", err)
		}
		state.HDDLED = boolPointer(active)
	}
	return state, nil
}

func (c *Controller) Changes() <-chan contract.ATXState { return c.changes }

func (c *Controller) watch(line gpioLine, stop <-chan struct{}) {
	for {
		select {
		case <-stop:
			return
		case _, ok := <-line.Changes():
			if !ok {
				return
			}
			state, err := c.State()
			if err != nil {
				continue
			}
			c.mu.Lock()
			changed := !c.lastSet || !sameState(c.last, state)
			if changed {
				c.last, c.lastSet = state, true
			}
			c.mu.Unlock()
			if changed {
				select {
				case c.changes <- state:
				default:
				}
			}
		}
	}
}

func sameState(left, right contract.ATXState) bool {
	return sameOptionalBool(left.PWRLED, right.PWRLED) && sameOptionalBool(left.HDDLED, right.HDDLED)
}

func sameOptionalBool(left, right *bool) bool {
	if left == nil || right == nil {
		return left == nil && right == nil
	}
	return *left == *right
}

func boolPointer(value bool) *bool { return &value }

func (c *Controller) Close() error {
	c.actionMu.Lock()
	defer c.actionMu.Unlock()
	c.mu.Lock()
	old, stop := c.lines, c.stop
	c.lines, c.stop = lines{}, nil
	c.mu.Unlock()
	if stop != nil {
		close(stop)
	}
	old.close()
	return nil
}
