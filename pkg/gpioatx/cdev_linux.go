//go:build linux

package gpioatx

import (
	"encoding/binary"
	"fmt"
	"sync"
	"unsafe"

	"github.com/onekvm/onekvm-plugins-gpio/pkg/contract"
	"golang.org/x/sys/unix"
)

const (
	gpioV2LineFlagActiveLow   = uint64(1 << 1)
	gpioV2LineFlagInput       = uint64(1 << 2)
	gpioV2LineFlagOutput      = uint64(1 << 3)
	gpioV2LineFlagEdgeRising  = uint64(1 << 4)
	gpioV2LineFlagEdgeFalling = uint64(1 << 5)
	gpioV2LineFlagOpenDrain   = uint64(1 << 6)
)

type cdevLine struct {
	mu      sync.Mutex
	fd      int
	stopFD  int
	input   bool
	changes chan struct{}
	done    chan struct{}
	once    sync.Once
}

func openCDEVLine(config contract.GPIOLineConfig, output bool) (gpioLine, error) {
	chipFD, err := unix.Open(config.Chip, unix.O_RDONLY|unix.O_CLOEXEC, 0)
	if err != nil {
		return nil, err
	}
	defer unix.Close(chipFD)

	request := unix.GPIOV2LineRequest{Num_lines: 1}
	request.Offsets[0] = config.Offset
	copy(request.Consumer[:], "onekvm-gpio-atx")
	if output {
		request.Config.Flags = gpioV2LineFlagOutput
		if config.OpenDrain {
			request.Config.Flags |= gpioV2LineFlagOpenDrain
		}
	} else {
		request.Config.Flags = gpioV2LineFlagInput | gpioV2LineFlagEdgeRising | gpioV2LineFlagEdgeFalling
		request.Event_buffer_size = 16
	}
	if config.ActiveLow {
		request.Config.Flags |= gpioV2LineFlagActiveLow
	}
	if err := ioctl(chipFD, unix.GPIO_V2_GET_LINE_IOCTL, unsafe.Pointer(&request)); err != nil {
		return nil, err
	}
	if request.Fd < 0 {
		return nil, fmt.Errorf("kernel returned invalid GPIO line descriptor")
	}
	line := &cdevLine{fd: int(request.Fd), stopFD: -1, input: !output, changes: make(chan struct{}, 1)}
	if line.input {
		line.stopFD, err = unix.Eventfd(0, unix.EFD_CLOEXEC|unix.EFD_NONBLOCK)
		if err != nil {
			_ = unix.Close(line.fd)
			return nil, fmt.Errorf("create GPIO event stop descriptor: %w", err)
		}
		line.done = make(chan struct{})
		go line.readEvents()
	}
	return line, nil
}

func (l *cdevLine) descriptor() int {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.fd
}

func (l *cdevLine) Get() (bool, error) {
	fd := l.descriptor()
	if fd < 0 {
		return false, unix.EBADF
	}
	values := unix.GPIOV2LineValues{Mask: 1}
	if err := ioctl(fd, unix.GPIO_V2_LINE_GET_VALUES_IOCTL, unsafe.Pointer(&values)); err != nil {
		return false, err
	}
	return values.Bits&1 != 0, nil
}

func (l *cdevLine) Set(active bool) error {
	fd := l.descriptor()
	if fd < 0 {
		return unix.EBADF
	}
	values := unix.GPIOV2LineValues{Mask: 1}
	if active {
		values.Bits = 1
	}
	return ioctl(fd, unix.GPIO_V2_LINE_SET_VALUES_IOCTL, unsafe.Pointer(&values))
}

func (l *cdevLine) Changes() <-chan struct{} { return l.changes }

func (l *cdevLine) readEvents() {
	defer close(l.changes)
	defer close(l.done)
	buffer := make([]byte, int(unsafe.Sizeof(unix.GPIOV2LineEvent{})))
	pollFDs := []unix.PollFd{{Fd: int32(l.fd), Events: unix.POLLIN}, {Fd: int32(l.stopFD), Events: unix.POLLIN}}
	for {
		if _, err := unix.Poll(pollFDs, -1); err != nil {
			if err == unix.EINTR {
				continue
			}
			return
		}
		if pollFDs[1].Revents != 0 {
			return
		}
		if pollFDs[0].Revents&unix.POLLIN == 0 {
			continue
		}
		if _, err := unix.Read(l.fd, buffer); err != nil {
			return
		}
		select {
		case l.changes <- struct{}{}:
		default:
		}
	}
}

func (l *cdevLine) Close() error {
	var err error
	l.once.Do(func() {
		if l.input {
			buffer := make([]byte, 8)
			binary.LittleEndian.PutUint64(buffer, 1)
			_, _ = unix.Write(l.stopFD, buffer)
			<-l.done
		}
		l.mu.Lock()
		fd := l.fd
		stopFD := l.stopFD
		l.fd = -1
		l.stopFD = -1
		l.mu.Unlock()
		if fd >= 0 {
			err = unix.Close(fd)
		}
		if stopFD >= 0 {
			_ = unix.Close(stopFD)
		}
		if !l.input {
			close(l.changes)
		}
	})
	return err
}

func ioctl(fd int, request uint, pointer unsafe.Pointer) error {
	_, _, errno := unix.Syscall(unix.SYS_IOCTL, uintptr(fd), uintptr(request), uintptr(pointer))
	if errno != 0 {
		return errno
	}
	return nil
}
