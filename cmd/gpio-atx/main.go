package main

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"os/signal"
	"syscall"

	"github.com/godbus/dbus/v5"
	"github.com/onekvm/onekvm-plugins-gpio/pkg/contract"
	"github.com/onekvm/onekvm-plugins-gpio/pkg/service"
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "onekvm-device-gpio-atx:", err)
		os.Exit(1)
	}
}

func run() error {
	provider, err := service.New()
	if err != nil {
		return err
	}
	defer provider.Close()
	conn, err := dbus.ConnectSystemBus()
	if err != nil {
		return fmt.Errorf("connect system D-Bus: %w", err)
	}
	defer conn.Close()
	if err := conn.Export(provider, service.ObjectPath, contract.Interface); err != nil {
		return fmt.Errorf("export GPIO ATX service: %w", err)
	}
	reply, err := conn.RequestName(service.BusName, dbus.NameFlagDoNotQueue)
	if err != nil {
		return fmt.Errorf("request D-Bus name: %w", err)
	}
	if reply != dbus.RequestNameReplyPrimaryOwner {
		return fmt.Errorf("D-Bus name %s is already owned", service.BusName)
	}
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	go func() {
		for {
			select {
			case <-ctx.Done():
				return
			case state := <-provider.Changes():
				payload, marshalErr := json.Marshal(state)
				if marshalErr == nil {
					_ = conn.Emit(service.ObjectPath, contract.Interface+".ATXStateChanged", string(payload))
				}
			}
		}
	}()
	<-ctx.Done()
	_, _ = conn.ReleaseName(service.BusName)
	return nil
}
