# USBHostProxy Library for Teensy 4.1

A USB HID mouse proxy library for Teensy 4.1 that enables mouse data forwarding with support for multiple input protocols.

## Features

- USB HID mouse device enumeration and data capture
- HID descriptor parsing with automatic format detection
- Multiple input protocol support via serial interface
- Persistent configuration storage in EEPROM
- Debug mode with detailed logging
- DevTools command interface for diagnostics

## Hardware Requirements

- Teensy 4.1 with USB Host cable
- Serial connection on Serial4 (TX pin 17, RX pin 16)
- USB HID mouse device

## Library Structure

```
USBHostProxy/
├── library.properties        # Arduino library metadata
├── README.md                # This documentation
├── src/                     # Core library components
│   ├── USBHostDriver.cpp/h  # USB host driver implementation
│   ├── HIDMouseDescriptorHandler.cpp/h  # HID descriptor parsing
│   ├── SunBoxEEPROM.cpp/h   # EEPROM configuration storage
│   ├── SunBoxStartup.cpp/h  # Early initialization
│   ├── USBDeviceProxy.cpp/h # USB device proxy (presents cloned device to host PC)
│   ├── SunBoxLogger.cpp/h   # Logging system with channel filtering and compile-time toggle
│   └── SunBoxAuth.cpp/h     # Authentication module
└── examples/
    └── SunshineUSBProxy/    # Example implementation
        ├── SunshineUSBProxy.ino  # Main sketch
        ├── SunBoxCommands.cpp/h  # Command routing system
        ├── CommandsSunBoxInterface.cpp/h  # Legacy protocol handler
        ├── CommandsSunBoxKMBoxInterface.cpp/h  # KMBox protocol handler
        ├── CommandsSunBoxDevtoolsInterface.cpp/h  # DevTools commands
        ├── SunBoxUSBMouseDataHandler.cpp/h  # USB mouse data processor
        └── SunBoxSyntheticHandleOutput.cpp/h  # Output mixer
```

## Core Library Components

### USBHostDriver

The main USB host driver that:
- Enumerates USB devices
- Claims HID mouse interfaces
- Manages USB pipes for data transfer
- Provides device information dumping

### HIDMouseDescriptorHandler

Parses HID report descriptors to:
- Extract mouse data field locations (buttons, X, Y, wheel)
- Support both standard boot protocol and custom descriptors
- Convert between raw HID data and MouseState structure

### SunBoxEEPROM

Manages persistent configuration:
- Debug mode enable/disable
- Forced interface selection for specific devices
- Configuration survives power cycles

### SunBoxStartup

Provides early initialization:
- Called via startup_early_hook() before main()
- Initializes Serial4 at 115200 baud
- Loads debug mode from EEPROM

### USBDeviceProxy

Custom polling-based USB device stack that presents the proxy device to the host PC:
- Handles descriptor forwarding from the downstream USB device
- Proxies control transfers between host and device
- Configures endpoints to mirror the downstream device
- Caches USB descriptors for fast re-enumeration

### SunBoxLogger

Structured logging with channel-based filtering:
- Channels: BOOT, CONNECT, ENUM, DATA, COMMAND, STATS, ERROR
- Channel mask is persisted to EEPROM (default: ERROR-only)
- Compile-time toggle: set `#define SUNBOX_LOGGING 0` in `SunBoxLogger.h` to strip ALL logging from the binary with zero overhead
- When `SUNBOX_LOGGING` is 1, individual channels can be enabled/disabled at runtime

### SunBoxAuth

Authentication module for securing the proxy device interface.

## Example Implementation: SunshineUSBProxy

The example sketch demonstrates a complete USB mouse proxy implementation with multiple input sources.

### Main Components

**SunBoxCommands** - Central command router that:
- Monitors Serial4 for incoming data
- Automatically detects protocol format
- Routes data to appropriate handler

**Protocol Handlers:**
- **CommandsSunBoxInterface** - Sunshine legacy 9-byte protocol
- **CommandsSunBoxKMBoxInterface** - KMBox B+ text commands
- **CommandsSunBoxDevtoolsInterface** - Built-in diagnostic commands

**SunBoxUSBMouseDataHandler** - Processes USB mouse data:
- Receives callbacks from USBHostDriver
- Uses HIDMouseDescriptorHandler to parse data
- Provides MouseState output

**SunBoxSyntheticHandleOutput** - Combines inputs:
- Mixes USB and serial mouse data
- Supports multiple mixing modes
- Outputs final mouse state

### Message Format

All serial output uses a prefix system:
- `S:` - System/startup messages (always shown)
- `I:` - Information/debug messages (shown when debug enabled)
- `E:` - Error messages (always shown)

These prefixes are now routed through SunBoxLogger with channel gating, so output for a given prefix only appears if the corresponding channel is enabled.

### Supported Protocols

#### Sunshine Legacy Protocol
- 9-byte format: `[length][data_bytes]`
- Length byte indicates payload size (3 or 8)
- 8-byte payload contains HID report data

#### KMBox B+ Protocol  
- Text-based commands starting with `km.`
- Command format: `km.command(parameters)`
- Line-based with timeout detection

#### DevTools Commands

| Command | Description |
|---------|-------------|
| `help` | Show available commands |
| `status` | Show current system status |
| `debug` | Toggle debug mode (saved to EEPROM) |
| `dump` | Display connected USB device descriptors |
| `claimcorrection vid,pid,interface,endpoint` | Force interface selection |
| `claimclear` | Clear forced interface selection |

### Data Flow

1. **USB Mouse Input**: Physical Mouse → USBHostDriver → HIDMouseDescriptorHandler → SunBoxUSBMouseDataHandler

2. **Serial Input**: Serial4 → SunBoxCommands → Protocol Handler → MouseState

3. **Mixed Output**: Both inputs → SunBoxSyntheticHandleOutput → Final output

## Usage

### Basic Setup

```cpp
#include <USBHost_t36.h>
#include "USBHostDriver.h"
#include "HIDMouseDescriptorHandler.h"

USBHost myusb;
USBHostDriver usbHostDriver(myusb);
HIDMouseDescriptorHandler hidHandler;

void setup() {
    myusb.begin();
    usbHostDriver.begin();
    hidHandler.begin(&usbHostDriver);
}

void loop() {
    myusb.Task();
}
```

### Building the Example

1. Install the USBHostProxy library in your Arduino libraries folder
2. Open `examples/SunshineUSBProxy/SunshineUSBProxy.ino`
3. Select Tools → Board → Teensy 4.1
4. Compile and upload

### Serial Connection

Connect to Serial4 at 115200 baud:
- TX: Pin 17
- RX: Pin 16
- GND: Common ground

## EEPROM Configuration

The library stores configuration at these addresses:

| Offset | Size | Description |
|--------|------|-------------|
| 0x00 | 12 bytes | Forced interface configuration |
| 0x0C | 5 bytes | Debug mode configuration |
| 0x40 | 8 bytes | Log channel configuration (magic + channel mask) |

Configuration is loaded automatically at startup.

## Debug Output

Logging is managed by SunBoxLogger with per-channel control:
- Channels: BOOT, CONNECT, ENUM, DATA, COMMAND, STATS, ERROR
- Channel mask is persisted to EEPROM; default is ERROR-only
- Compile-time toggle: set `#define SUNBOX_LOGGING 0` in `SunBoxLogger.h` to strip ALL logging from the binary (zero overhead)
- When `SUNBOX_LOGGING` is 1, individual channels can be enabled/disabled at runtime via DevTools commands

Enabling relevant channels surfaces details such as:
- HID descriptor parsing details (ENUM channel)
- Mouse data packets (DATA channel)
- USB enumeration process (CONNECT / ENUM channels)
- Command routing decisions (COMMAND channel)

Example debug output:
```
I: === Parsing HID Report Descriptor ===
I:   [0] Usage Page: 0x1
I:   [2] Usage: 0x2 (Mouse)
S: >>> Found Buttons: offset=0 count=5 bits=5
S: >>> Found X: offset=16 bits=16 range=-32768..32767
S: >>> Found Y: offset=32 bits=16 range=-32768..32767
```

## LED Status

The built-in LED indicates:
- Slow blink: Waiting for USB device
- Solid on: USB device connected
- 5 quick flashes: USB device stack initialized

## Troubleshooting

**No USB device detected:**
- Check USB host cable connection
- Verify mouse works on a computer
- Use `dump` command to check enumeration

**Wrong interface claimed:**
- Use `dump` to list all interfaces
- Use `claimcorrection` to force correct interface
- Power cycle after configuration change

**No serial output:**
- Verify Serial4 connections
- Check baud rate (115200)
- Ensure common ground connection

## Technical Specifications

- USB Host polling: 1ms intervals
- Serial baud rate: 115200
- Maximum buttons: 8
- Movement range: -32768 to 32767
- EEPROM usage: 25 bytes (17 bytes existing + 8 bytes log channel config)

## Version Information

Current version: 3.1
- Clean architecture implementation
- Multi-protocol support
- Persistent configuration
- Logging channel system with compile-time toggle