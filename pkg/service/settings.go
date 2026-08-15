package service

import (
	"fmt"

	"github.com/onekvm/onekvm-plugins-gpio/pkg/contract"
)

type Settings struct {
	UseDefault bool `json:"use_default"`

	PowerButtonEnabled   bool   `json:"power_button_enabled"`
	PowerButtonChip      string `json:"power_button_chip"`
	PowerButtonOffset    int    `json:"power_button_offset"`
	PowerButtonActiveLow bool   `json:"power_button_active_low"`
	PowerButtonOpenDrain bool   `json:"power_button_open_drain"`

	ResetButtonEnabled   bool   `json:"reset_button_enabled"`
	ResetButtonChip      string `json:"reset_button_chip"`
	ResetButtonOffset    int    `json:"reset_button_offset"`
	ResetButtonActiveLow bool   `json:"reset_button_active_low"`
	ResetButtonOpenDrain bool   `json:"reset_button_open_drain"`

	PWRLEDEnabled   bool   `json:"pwr_led_enabled"`
	PWRLEDChip      string `json:"pwr_led_chip"`
	PWRLEDOffset    int    `json:"pwr_led_offset"`
	PWRLEDActiveLow bool   `json:"pwr_led_active_low"`

	HDDLEDEnabled   bool   `json:"hdd_led_enabled"`
	HDDLEDChip      string `json:"hdd_led_chip"`
	HDDLEDOffset    int    `json:"hdd_led_offset"`
	HDDLEDActiveLow bool   `json:"hdd_led_active_low"`

	PowerShortMS int `json:"power_short_ms"`
	PowerLongMS  int `json:"power_long_ms"`
	ResetPulseMS int `json:"reset_pulse_ms"`
}

func settingsFromConfig(config contract.ATXConfig, useDefault bool) Settings {
	result := Settings{
		UseDefault: useDefault, PowerShortMS: config.PowerShortMS,
		PowerLongMS: config.PowerLongMS, ResetPulseMS: config.ResetPulseMS,
	}
	if result.PowerShortMS == 0 {
		result.PowerShortMS = 200
	}
	if result.PowerLongMS == 0 {
		result.PowerLongMS = 5000
	}
	if result.ResetPulseMS == 0 {
		result.ResetPulseMS = 200
	}
	if line := config.PowerButton; line != nil {
		result.PowerButtonEnabled = true
		result.PowerButtonChip, result.PowerButtonOffset = line.Chip, int(line.Offset)
		result.PowerButtonActiveLow, result.PowerButtonOpenDrain = line.ActiveLow, line.OpenDrain
	}
	if line := config.ResetButton; line != nil {
		result.ResetButtonEnabled = true
		result.ResetButtonChip, result.ResetButtonOffset = line.Chip, int(line.Offset)
		result.ResetButtonActiveLow, result.ResetButtonOpenDrain = line.ActiveLow, line.OpenDrain
	}
	if line := config.PWRLED; line != nil {
		result.PWRLEDEnabled = true
		result.PWRLEDChip, result.PWRLEDOffset = line.Chip, int(line.Offset)
		result.PWRLEDActiveLow = line.ActiveLow
	}
	if line := config.HDDLED; line != nil {
		result.HDDLEDEnabled = true
		result.HDDLEDChip, result.HDDLEDOffset = line.Chip, int(line.Offset)
		result.HDDLEDActiveLow = line.ActiveLow
	}
	return result
}

func (s Settings) config() (contract.ATXConfig, error) {
	for name, value := range map[string]int{
		"power_button_offset": s.PowerButtonOffset,
		"reset_button_offset": s.ResetButtonOffset,
		"pwr_led_offset":      s.PWRLEDOffset,
		"hdd_led_offset":      s.HDDLEDOffset,
	} {
		if value < 0 || value > int(^uint32(0)) {
			return contract.ATXConfig{}, fmt.Errorf("%s must be between 0 and %d", name, uint64(^uint32(0)))
		}
	}
	for name, value := range map[string]int{
		"power_short_ms": s.PowerShortMS,
		"power_long_ms":  s.PowerLongMS,
		"reset_pulse_ms": s.ResetPulseMS,
	} {
		if value < 1 || value > 30_000 {
			return contract.ATXConfig{}, fmt.Errorf("%s must be between 1 and 30000", name)
		}
	}
	line := func(enabled bool, chip string, offset int, activeLow, openDrain bool) (*contract.GPIOLineConfig, error) {
		if !enabled {
			return nil, nil
		}
		if chip == "" {
			return nil, fmt.Errorf("enabled GPIO line requires a chip path")
		}
		return &contract.GPIOLineConfig{Chip: chip, Offset: uint32(offset), ActiveLow: activeLow, OpenDrain: openDrain}, nil
	}
	var config contract.ATXConfig
	var err error
	if config.PowerButton, err = line(s.PowerButtonEnabled, s.PowerButtonChip, s.PowerButtonOffset, s.PowerButtonActiveLow, s.PowerButtonOpenDrain); err != nil {
		return config, fmt.Errorf("power button: %w", err)
	}
	if config.ResetButton, err = line(s.ResetButtonEnabled, s.ResetButtonChip, s.ResetButtonOffset, s.ResetButtonActiveLow, s.ResetButtonOpenDrain); err != nil {
		return config, fmt.Errorf("reset button: %w", err)
	}
	if config.PWRLED, err = line(s.PWRLEDEnabled, s.PWRLEDChip, s.PWRLEDOffset, s.PWRLEDActiveLow, false); err != nil {
		return config, fmt.Errorf("PWR LED: %w", err)
	}
	if config.HDDLED, err = line(s.HDDLEDEnabled, s.HDDLEDChip, s.HDDLEDOffset, s.HDDLEDActiveLow, false); err != nil {
		return config, fmt.Errorf("HDD LED: %w", err)
	}
	config.PowerShortMS, config.PowerLongMS, config.ResetPulseMS = s.PowerShortMS, s.PowerLongMS, s.ResetPulseMS
	return config, nil
}
