package contract

const (
	APIMajor  = 1
	APIMinor  = 2
	Interface = "org.onekvm.Device1"
)

type Capabilities struct {
	APIMajor int  `json:"api_major"`
	APIMinor int  `json:"api_minor"`
	ATX      bool `json:"atx,omitempty"`
}

type GPIOLineConfig struct {
	Chip      string `json:"chip"`
	Offset    uint32 `json:"offset"`
	ActiveLow bool   `json:"active_low,omitempty"`
	OpenDrain bool   `json:"open_drain,omitempty"`
}

type ATXConfig struct {
	PowerButton  *GPIOLineConfig `json:"power_button,omitempty"`
	ResetButton  *GPIOLineConfig `json:"reset_button,omitempty"`
	PWRLED       *GPIOLineConfig `json:"pwr_led,omitempty"`
	HDDLED       *GPIOLineConfig `json:"hdd_led,omitempty"`
	PowerShortMS int             `json:"power_short_ms,omitempty"`
	PowerLongMS  int             `json:"power_long_ms,omitempty"`
	ResetPulseMS int             `json:"reset_pulse_ms,omitempty"`
}

type ATXState struct {
	PWRLED *bool `json:"pwr_led,omitempty"`
	HDDLED *bool `json:"hdd_led,omitempty"`
}
