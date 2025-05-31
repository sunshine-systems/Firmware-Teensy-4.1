# SunBox USB Proxy System

## Overview

The SunBox USB Proxy System is a modular USB proxy implementation for Teensy 4.1 that creates a transparent bridge between a USB host (computer) and a USB HID device. The system intercepts and can modify USB HID data while maintaining full device functionality. It uses a structured startup sequence with clear separation of concerns and implements a state machine for reliable device handling.

## Key Features

- **Transparent USB Proxy**: Device appears identical to the host system
- **HID Device Support**: Handles mice, keyboards, and other HID devices
- **Boot Protocol Fallback**: Automatically uses boot protocol when HID descriptors are unavailable
- **State Machine Control**: Reliable device detection and initialization sequence
- **Delayed USB Initialization**: USB device stack only starts after HID device is fully understood
- **Comprehensive Logging**: Detailed debug output on Serial4 for troubleshooting
- **Runtime Configuration**: Debug commands available via Serial4

## Architecture

### Core Components

1. **startup.c** - Low-level system initialization
2. **SunBoxStartup** - Early initialization helper (Serial4 only)
3. **USBHostDriver** - USB host operations and device management
4. **HIDReportParser** - HID descriptor parsing and data conversion
5. **Main Sketch** - Application logic and state machine

### Component Responsibilities

#### startup.c
- Hardware initialization (CPU, clocks, memory)
- Calls `SunBoxStartup_begin()` for early Serial4 setup
- **Does NOT call usb_init()** - this is handled by main sketch
- Manages low-level boot sequence

#### SunBoxStartup (C++ with C interface)
- Minimal initialization - only sets up Serial4
- Provides C-callable interface for startup.c
- No USB object creation (handled by main sketch)

#### USBHostDriver
- Inherits from USBHost_t36's USBDriver class
- Self-registers with USBHost in constructor
- Claims USB devices based on interface descriptors
- Parses descriptors to find HID endpoints
- Manages interrupt endpoint data transfers
- Provides both callback and polling interfaces
- Calculates actual transfer lengths from EHCI tokens

#### HIDReportParser
- Parses HID report descriptors
- Automatically falls back to boot mouse protocol
- Converts between raw HID data and MouseState structures
- Supports various mouse formats
- Provides detailed descriptor analysis

#### Main Sketch (State Machine)
- Creates all USB objects as global direct instances
- Implements state machine for initialization flow
- Delays USB device initialization until HID is understood
- Monitors device connection/disconnection
- Provides debug command interface
- Manages LED status indicators

## State Machine

The system uses a state machine to ensure proper initialization order:

```
INIT
 └─> WAIT_FOR_DEVICE (Slow LED blink)
      └─> DEVICE_DETECTED (Device claimed)
           └─> HID_PARSED (HID descriptor analyzed)
                └─> USB_DEVICE_READY (USB device stack initialized)
                     └─> PROXY_ACTIVE (Solid LED, full operation)
```

### State Descriptions

- **INIT**: Initial state, system starting up
- **WAIT_FOR_DEVICE**: USB Host active, waiting for device connection
- **DEVICE_DETECTED**: Device connected and claimed by driver
- **HID_PARSED**: HID descriptor parsed or boot protocol selected
- **USB_DEVICE_READY**: USB device stack initialized (usb_init() called)
- **PROXY_ACTIVE**: Full proxy operation, data flowing bidirectionally

## Initialization Flow

```
1. Hardware Reset
   └─> startup.c: ResetHandler()
       ├─> Hardware initialization
       ├─> SunBoxStartup_begin() [Serial4 only]
       └─> main() → setup() → loop()

2. Arduino setup()
   ├─> Create USB objects (direct instances, not pointers):
   │   ├─> USBHost myusb
   │   ├─> USBHostDriver usbHostDriver(myusb) [auto-registers]
   │   └─> HIDReportParser hidReportParser
   ├─> Configure components
   ├─> myusb.begin() [Start USB Host]
   └─> Enter WAIT_FOR_DEVICE state

3. Device Connection (main loop)
   └─> USBHost detects device
       └─> USBHostDriver::claim() called
           ├─> Type 0: Store device reference
           └─> Type 1: Claim interface
               ├─> Parse descriptors
               ├─> Create interrupt pipes
               └─> Start data reception
                   └─> State → DEVICE_DETECTED

4. HID Analysis (main loop)
   └─> Attempt to get HID descriptor
       ├─> Parse if available
       └─> Use boot protocol if not
           └─> State → HID_PARSED

5. USB Device Initialization (main loop)
   └─> Call usb_init() [ONLY NOW!]
       └─> State → USB_DEVICE_READY
           └─> State → PROXY_ACTIVE

6. Data Flow (PROXY_ACTIVE state)
   └─> Device → USBHostDriver → HIDReportParser → [Modification] → USB Device → Host
```

## Critical Design Decisions

### 1. Direct Object Creation
Objects are created as direct instances, not pointers:
```cpp
USBHost myusb;                    // Direct object
USBHostDriver usbHostDriver(myusb);  // Direct object
HIDReportParser hidReportParser;      // Direct object
```
This ensures proper initialization order and stable memory addresses.

### 2. Constructor Registration
USBHostDriver registers itself with USBHost in its constructor, ensuring it's ready before USB Host starts:
```cpp
USBHostDriver::USBHostDriver(USBHost& host) {
    // ... initialization ...
    driver_ready_for_device(this);  // Register BEFORE USBHost::begin()
}
```

### 3. Delayed USB Device Initialization
`usb_init()` is called ONLY after the HID device is fully understood. This ensures the USB device stack can be configured to match the connected device.

## Logging Structure

### Log Prefixes
- `[STARTUP]:` - USBHostDriver and startup messages
- `[MAIN]:` - Main sketch state machine and status
- `[PARSER]:` - HID report parser messages

### Example Boot Sequence
```
[STARTUP]: SunBox early initialization complete.
[STARTUP]: USBHostDriver constructor called
[STARTUP]: Contributing Pipes and Transfers to USB Host
[STARTUP]: Registering driver with USB Host (in constructor)
[MAIN]: USB Host Driver created (registered in constructor)
[MAIN]: Starting USB Host...
[MAIN]: Setup complete - waiting for device...
[STARTUP]: USBHostDriver::claim() called - type: 0, len: 75
[STARTUP]: Device VID: 0x3662, PID: 0x2004
[STARTUP]: Interface level claim (type 1) - claiming device!
[STARTUP]: Found endpoint: addr=0x81 attr=0x3 size=8 interval=1
[STARTUP]: Device successfully claimed!
[MAIN]: Device detected and claimed!
[MAIN]: HID device ready - initializing USB device stack...
[MAIN]: Proxy fully initialized and ready!
```

## Debug Interface

### Serial4 Commands (115200 baud)
- `d` - Toggle debug mode
- `s` - Show system status
- `h` or `?` - Show help

### Status Display
```
=== System Status ===
State: PROXY_ACTIVE
Device: Connected (VID:0x3662 PID:0x2004)
HID Parser: Parsed
USB Device Stack: Initialized
Debug Mode: OFF
====================
```

### LED Status Indicators
- **Slow blink** (1Hz): Waiting for device
- **Fast blink** (5Hz): Device detected, initializing
- **Solid on**: Proxy active and operational
- **Off**: Error or init state

## Data Structures

### MouseState
```cpp
struct MouseState {
    int16_t x;        // X-axis movement
    int16_t y;        // Y-axis movement  
    int8_t wheel;     // Wheel movement
    uint8_t buttons;  // Button states
    
    bool leftButton() const { return buttons & 0x01; }
    bool rightButton() const { return buttons & 0x02; }
    bool middleButton() const { return buttons & 0x04; }
};
```

## Hardware Configuration

### Teensy 4.1 Connections
- **USB Host Port**: Connect HID device here
- **USB Device Port**: Connect to computer
- **Serial4** (Pin 17 TX, Pin 16 RX): Debug interface
- **LED_BUILTIN** (Pin 13): Status indicator

## Building and Installation

1. Install Arduino IDE with Teensyduino
2. Install USBHost_t36 library
3. Copy all project files to sketch folder
4. Select Teensy 4.1 board
5. Build and upload

## Troubleshooting

### Device Not Detected
1. Check USB Host port connection
2. Verify device is HID class
3. Monitor Serial4 for claim() calls
4. Check LED blink pattern

### Claim Not Happening
1. Ensure USBHostDriver is created before myusb.begin()
2. Verify virtual function overrides use `override` keyword
3. Check that USBHostDriver inherits from USBDriver

### USB Device Stack Issues
1. Verify usb_init() is called only in USB_DEVICE_READY state
2. Check that HID parsing completed successfully
3. Monitor Serial4 for initialization sequence

## Future Enhancements

1. **Multi-Device Support**
   - Handle multiple HID devices simultaneously
   - USB hub support

2. **Extended HID Support**
   - Keyboards with full NKRO
   - Game controllers
   - Digitizers and touchscreens

3. **Data Modification Framework**
   - Plugin architecture for data transforms
   - Macro recording and playback
   - Input remapping

4. **Performance Optimization**
   - Zero-copy data paths
   - DMA transfer optimization
   - Reduced interrupt latency

## Technical Notes

### EHCI Transfer Length Calculation
The driver correctly calculates actual transfer lengths from EHCI queue Transfer Descriptor (qTD) tokens:
```cpp
uint32_t bytes_not_transferred = (token >> 16) & 0x7FFF;
uint32_t actual_len = requested_len - bytes_not_transferred;
```

### Boot Protocol Fallback
When HID descriptors are unavailable, the parser automatically configures for standard boot mouse protocol:
- 3-byte minimum reports
- Byte 0: Buttons
- Byte 1: X movement (signed)
- Byte 2: Y movement (signed)

### Memory Alignment
All USB buffers are 32-byte aligned as required by the USB controller:
```cpp
uint8_t buffer[64] __attribute__ ((aligned(32)));
```