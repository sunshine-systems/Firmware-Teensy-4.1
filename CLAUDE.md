# USB Host Proxy Library Documentation

## Overview

The USB Host Proxy is a sophisticated library for Teensy 4.1 that creates a transparent bridge between a USB HID mouse and a host PC. The device appears identical to the physical mouse, supporting high-frequency polling rates up to 8kHz and handling various USB speeds and device quirks.

## Architecture

### Core Components

1. **USBDeviceProxy** (`src/USBDeviceProxy.cpp/h`)
   - Custom polling-based USB device stack (no interrupts)
   - Dynamically matches USB speed of connected device
   - Handles control transfers and data forwarding
   - Supports Low Speed (1.5 Mbps), Full Speed (12 Mbps), and High Speed (480 Mbps)

2. **USBHostDriver** (`src/USBHostDriver.cpp/h`)
   - USB host implementation using USBHost_t36
   - Enumerates and claims HID mouse interfaces
   - Manages USB pipes and data transfer
   - Provides device information and speed detection

3. **HIDMouseDescriptorHandler** (`src/HIDMouseDescriptorHandler.cpp/h`)
   - Parses HID report descriptors
   - Extracts mouse data field locations (buttons, X, Y, wheel)
   - Converts between raw HID data and MouseState structures

4. **SunBoxStartup** (`src/SunBoxStartup.cpp/h`)
   - Early initialization via modified `startup.c`
   - Called before main() to initialize Serial4
   - Loads debug configuration from EEPROM

5. **SunBoxEEPROM** (`src/SunBoxEEPROM.cpp/h`)
   - Persistent configuration storage
   - Debug mode settings
   - Interface override for specific devices

## Data Flow

```
Physical Mouse → USB Host (Teensy) → Processing → USB Device (Teensy) → Host PC
                     ↓                    ↓                ↓
              USBHostDriver         Command Handlers  USBDeviceProxy
                     ↓                    ↓                ↓
              HID Descriptor         Serial Input    Endpoint Mapping
                 Parsing             Processing      & Forwarding
```

## Key Features

### 1. Transparent Device Emulation
- All USB descriptors proxied 1:1 from physical device
- PC sees exact replica of physical mouse
- Maintains vendor/product IDs and all device characteristics

### 2. Dynamic Speed Matching
- Automatically detects device speed (Low/Full/High)
- Low Speed devices transparently converted to Full Speed
- EP0 packet size dynamically configured (8 or 64 bytes)

### 3. High-Performance Polling
- Achieves true 8kHz polling for gaming mice
- Polling-based architecture prevents interrupt conflicts
- Sub-millisecond latency end-to-end

### 4. Multi-Protocol Support
The example implementation (`examples/SunshineUSBProxy/`) supports multiple input protocols:

- **Sunshine Legacy Protocol**: 9-byte binary format
- **DevTools Commands**: Built-in diagnostics and configuration

### 5. Intelligent Device Handling
- Request filtering for non-compliant devices
- Dynamic endpoint mapping with EEPROM overrides
- Vendor control transfer support (SET_REPORT/GET_REPORT)

## Critical Implementation Details

### Modified startup.c
The library modifies `cores/teensy4/startup.c` to add early initialization:
- Lines 62-69: USB Host Proxy globals
- Line 9: Include SunBoxStartup.h
- Line 214: Call SunBoxStartup_begin() during startup

### Endpoint Configuration
The most critical discovery was the ZLT bit (bit 29) in endpoint configuration:
```cpp
uint32_t config = (maxPacket << 16) | (1 << 29);  // ZLT bit prevents stalls
```

### Speed Configuration
USB PHY must be configured before controller initialization:
```cpp
// Force Full Speed (12 Mbps)
USBPHY1_CTRL_CLR = USBPHY_CTRL_ENUTMILEVEL2 | USBPHY_CTRL_ENUTMILEVEL3;

// Force High Speed (480 Mbps)  
USBPHY1_CTRL_SET = USBPHY_CTRL_ENUTMILEVEL2 | USBPHY_CTRL_ENUTMILEVEL3;
```

### Non-Compliant Device Support
Request filtering handles devices that STALL on optional requests:
```cpp
// Device Qualifier - Full Speed devices can legitimately STALL
if (desc_type == 0x06) {  // Device Qualifier
    USB1_ENDPTCTRL0 = 0x00010001;  // STALL
    return;
}
```

## Example Usage

### Basic Setup
```cpp
#include <USBHost_t36.h>
#include "USBHostDriver.h"
#include "USBDeviceProxy.h"

USBHost myusb;
USBHostDriver usbHostDriver(myusb);
USBDeviceProxy usbDeviceProxy;

void setup() {
    myusb.begin();
    usbHostDriver.begin();
    
    // Wait for mouse detection
    while (!usbHostDriver.isReady()) {
        myusb.Task();
    }
    
    // Configure proxy to match device speed
    uint8_t speed = usbHostDriver.getDeviceSpeed();
    usbDeviceProxy.setDeviceSpeed(speed != 0);  // true for High Speed
    usbDeviceProxy.setUSBHostDriver(&usbHostDriver);
    usbDeviceProxy.begin();
}

void loop() {
    myusb.Task();
    usbDeviceProxy.poll();  // Must be called frequently!
}
```

### DevTools Commands

| Command | Description |
|---------|-------------|
| `help` | Show available commands |
| `status` | Display system status |
| `debug` | Toggle debug mode |
| `dump` | Show USB device descriptors |
| `claimcorrection vid,pid,iface,ep` | Force interface selection |
| `claimclear` | Clear forced selection |

## Serial Communication

All communication uses Serial4 (115200 baud):
- TX: Pin 17
- RX: Pin 16

Output prefixes:
- `S:` System/startup messages
- `I:` Information/debug messages
- `E:` Error messages

## EEPROM Configuration

| Address | Size | Purpose |
|---------|------|---------|
| 0x00 | 12 bytes | Interface override config |
| 0x0C | 5 bytes | Debug mode settings |

## Verified Devices

### Low Speed (1.5 → 12 Mbps)
- BenQ ZOWIE (0x04A5:0x8001)

### Full Speed (12 Mbps)
- Glorious Model O Wireless (0x258A:0x2022)
- Logitech G307 (0x046D:0xC53F) - Requires EEPROM override

### High Speed (480 Mbps)
- Pwnage V3 (0x3662:0x2004) - 8kHz polling verified
- Logitech G Pro X Superlight
- Beast X Mini

## Troubleshooting

### No Device Detection
- Check USB host cable connection
- Verify mouse works on PC directly
- Use `dump` command to check enumeration

### Wrong Interface Claimed
- Use `dump` to list interfaces
- Use `claimcorrection` to force correct interface
- Power cycle after configuration

### Polling Issues
- "One packet then stall": Missing ZLT bit
- Speed mismatch: PHY configuration issue
- Endpoint not ready: Hardware signal issue

## Technical Specifications

- Polling Rate: ~200-400kHz main loop
- Latency: <1ms end-to-end
- USB Speeds: 1.5/12/480 Mbps
- HID Report Rate: Up to 8kHz
- Memory: 17 bytes EEPROM usage

## Building and Installation

1. Install USBHostProxy library in Arduino libraries folder
2. Open `examples/SunshineUSBProxy/SunshineUSBProxy.ino`
3. Select Tools → Board → Teensy 4.1
4. Compile and upload

Note: The modified `startup.c` is already included in this Teensy core installation.

## LED Status Indicators

- Slow blink: Waiting for USB device
- Fast blink: Mouse connected, PC not detecting
- Solid on: Fully connected and operational
- 5 quick flashes: USB device stack initialized

## Version Information

Current Version: 3.0
- Clean architecture implementation
- Multi-protocol support
- Universal device compatibility
- Vendor command support