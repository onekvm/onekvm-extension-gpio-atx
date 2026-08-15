package gpioatx

import (
	"encoding/json"
	"os"
	"testing"

	"github.com/onekvm/onekvm-plugins-gpio/pkg/contract"
)

// TestLiveConfigureAndRead is opt-in and non-destructive. It requests button
// outputs in their inactive state and only reads LED inputs; it never invokes
// an ATX action.
func TestLiveConfigureAndRead(t *testing.T) {
	payload := os.Getenv("ONEKVM_GPIO_ATX_LIVE_CONFIG")
	if payload == "" {
		t.Skip("ONEKVM_GPIO_ATX_LIVE_CONFIG is not set")
	}
	var config contract.ATXConfig
	if err := json.Unmarshal([]byte(payload), &config); err != nil {
		t.Fatal(err)
	}
	controller := NewController()
	defer controller.Close()
	if err := controller.Configure(config); err != nil {
		t.Fatal(err)
	}
	state, err := controller.State()
	if err != nil {
		t.Fatal(err)
	}
	t.Logf("ATX state: %+v", state)
}
