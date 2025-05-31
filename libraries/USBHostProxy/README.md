# SunBox USB Proxy System

## Overview

The SunBox USB Proxy System is a modular USB proxy implementation for Teensy 4.1 that creates a transparent bridge between a USB host (computer) and a USB HID device. The system intercepts and can modify USB HID data while maintaining full device functionality. It uses a structured startup sequence with clear separation of concerns and implements a state machine for reliable device handling.

## Key Features

- **Transparent USB Proxy**: Device appears identical to the host system
- **HID Device Support**: Handles mice, keyboards, and other HID devices
- **Active HID Descriptor Retrieval**: Uses USB control transfers to get device descriptors
- **Boot Protocol Fallback**: Automatically uses boot protocol when HID descriptors are unavailable
- **State Machine Control**: Reliable device detection and initialization sequence
- **Delayed USB Initialization**: USB device stack only starts after HID device is fully understood
- **Comprehensive Logging**: Detailed debug output on Serial4 for troubleshooting
- **Runtime Configuration**: Debug commands available via Serial4
- **Data Format Testing**: Built-in commands to test HID data formatting

## Architecture

### Core Components

1. **startup.c** - Low-level system initialization
2. **SunBoxStartup** - Early initialization helper (Serial4 only)
3. **USBHostDriver** - USB host operations with control transfer support
4. **HIDMouseDescriptorHandler** - HID descriptor retrieval, parsing and data conversion
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
- **Provides control transfer API for descriptor retrieval**
- Parses descriptors to find HID endpoints and interfaces
- Extracts HID descriptor lengths from HID class descriptors
- Manages interrupt endpoint data transfers
- Provides interface enumeration and query methods
- Calculates actual transfer lengths from EHCI tokens

#### HIDMouseDescriptorHandler
- **Uses USBHostDriver's control transfer API to retrieve HID descriptors**
- Finds and identifies mouse interfaces
- Requests HID report descriptors via USB control transfers
- Parses HID report descriptors
- Automatically falls back to boot mouse protocol
- Converts between raw HID data and MouseState structures
- Supports various mouse formats
- Provides detailed descriptor analysis

#### Main Sketch (State Machine)
- Creates all USB objects as global direct instances
- Implements state machine for initialization flow
- **Manages HID descriptor retrieval process**
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
           └─> HID_DESCRIPTOR_WAIT (Retrieving HID descriptor)
                └─> HID_PARSED (HID descriptor analyzed)
                     └─> USB_DEVICE_READY (USB device stack initialized)
                          └─> PROXY_ACTIVE (Solid LED, full operation)
```

### State Descriptions

- **INIT**: Initial state, system starting up
- **WAIT_FOR_DEVICE**: USB Host active, waiting for device connection
- **DEVICE_DETECTED**: Device connected and claimed by driver
- **HID_DESCRIPTOR_WAIT**: Requesting HID descriptor via control transfer
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
   │   └─> HIDMouseDescriptorHandler hidMouseHandler
   ├─> Initialize HID handler with USB driver reference
   ├─> Configure components
   ├─> myusb.begin() [Start USB Host]
   └─> Enter WAIT_FOR_DEVICE state

3. Device Connection (main loop)
   └─> USBHost detects device
       └─> USBHostDriver::claim() called
           ├─> Type 0: Store device reference
           └─> Type 1: Claim interface
               ├─> Parse descriptors
               ├─> Extract HID descriptor lengths
               ├─> Create interrupt pipes
               └─> Start data reception
                   └─> State → DEVICE_DETECTED

4. HID Descriptor Retrieval (main loop)
   └─> HIDMouseDescriptorHandler::setupMouseInterface()
       └─> Find mouse interface from driver
           └─> HIDMouseDescriptorHandler::requestHIDDescriptor()
               └─> USBHostDriver::controlTransfer()
                   └─> USB GET_DESCRIPTOR request
                       └─> State → HID_PARSED

5. USB Device Initialization (main loop)
   └─> Call usb_init() [ONLY NOW!]
       └─> State → USB_DEVICE_READY
           └─> State → PROXY_ACTIVE

6. Data Flow (PROXY_ACTIVE state)
   └─> Device → USBHostDriver → HIDMouseDescriptorHandler → [Modification] → USB Device → Host
```

## Critical Design Decisions

### 1. Direct Object Creation
Objects are created as direct instances, not pointers:
```cpp
USBHost myusb;                              // Direct object
USBHostDriver usbHostDriver(myusb);         // Direct object
HIDMouseDescriptorHandler hidMouseHandler;   // Direct object
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

### 3. Control Transfer Architecture
The system uses a clean separation of concerns for HID descriptor retrieval:
```cpp
HIDMouseDescriptorHandler → requests descriptor
    └─> USBHostDriver::controlTransfer() → USB communication
        └─> HIDMouseDescriptorHandler → parses result
```

### 4. Delayed USB Device Initialization
`usb_init()` is called ONLY after the HID device is fully understood. This ensures the USB device stack can be configured to match the connected device.

## Logging Structure

### Log Prefixes
- `[STARTUP]:` - USBHostDriver and startup messages
- `[MAIN]:` - Main sketch state machine and status
- `[HID_HANDLER]:` - HID descriptor handler messages
- `[DRIVER]:` - USB driver control transfer messages
- `[PARSER]:` - HID report parser messages
- `[DATA]:` - Data reception messages

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
[STARTUP]: Found interface 0, class: 0x3 (HID Mouse)
[STARTUP]: HID descriptor found! Report length: 91 for interface 0
[STARTUP]: Found endpoint: addr=0x81 attr=0x3 size=8 interval=1
[STARTUP]: Device successfully claimed!
[MAIN]: Device detected and claimed!
[MAIN]: Setting up HID mouse interface...
[HID_HANDLER]: Found mouse interface 0 at index 0
[MAIN]: Requesting HID descriptor...
[DRIVER]: Control transfer: bmRequestType=0x81 bRequest=0x6 wValue=0x2200 wIndex=0x0 wLength=91
[DRIVER]: Control transfer complete, received 91 bytes
[HID_HANDLER]: Retrieved 91 bytes of HID descriptor
=== Parsing HID Report Descriptor ===
[PARSER]: Found: Buttons X Y Wheel
[MAIN]: HID descriptor processed!
[MAIN]: HID device ready - initializing USB device stack...
[MAIN]: Proxy fully initialized and ready!
```

## Debug Interface

### Serial4 Commands (115200 baud)
- `1` - Test format: Move mouse left 10px
- `2` - Test format: Move mouse right 10px
- `d` - Toggle debug mode
- `s` - Show system status
- `i` - Show interface/descriptor info
- `h` or `?` - Show help

### Status Display
```
=== System Status ===
State: PROXY_ACTIVE
Device: Connected (VID:0x3662 PID:0x2004)
HID Handler: Ready (Interface 0, EP 0x81)
HID Parser: Parsed
USB Device Stack: Initialized
Debug Mode: OFF
====================
```

### LED Status Indicators
- **Slow blink** (1Hz): Waiting for device
- **Fast blink** (5Hz): Device detected, initializing, or retrieving descriptor
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

### Control Transfer API
```cpp
bool controlTransfer(
    uint8_t bmRequestType,  // Request type bitmap
    uint8_t bRequest,       // Request code
    uint16_t wValue,        // Value parameter
    uint16_t wIndex,        // Index parameter
    uint16_t wLength,       // Data length
    uint8_t* data,          // Data buffer
    uint16_t* actualLength, // Actual bytes received
    uint32_t timeout_ms     // Timeout in milliseconds
);
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

### HID Descriptor Retrieval Fails
1. Check Serial4 for control transfer messages
2. Verify interface has HID descriptor length
3. Some devices may not provide descriptors (will use boot protocol)
4. Check timeout values

### USB Device Stack Issues
1. Verify usb_init() is called only in USB_DEVICE_READY state
2. Check that HID parsing completed successfully
3. Monitor Serial4 for initialization sequence
4. Note: "endpoint 0 stall" errors are expected until usb.c is updated

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

### Control Transfer Implementation
The USBHostDriver provides a complete control transfer API that handles:
- Setup packet formation
- Data stage management
- Status stage completion
- Timeout handling
- Error detection

### HID Descriptor Retrieval
HID descriptors are retrieved using:
- bmRequestType: 0x81 (Device-to-Host, Standard, Interface)
- bRequest: 0x06 (GET_DESCRIPTOR)
- wValue: 0x2200 (Report descriptor type)
- wIndex: Interface number
- wLength: Descriptor length (from HID class descriptor)

### Boot Protocol Fallback
When HID descriptors are unavailable or unparseable, the system automatically configures for standard boot mouse protocol:
- 3-8 byte reports
- Byte 0: Buttons
- Byte 1: X movement (signed)
- Byte 2: Y movement (signed)
- Byte 3: Wheel (optional)

### Memory Alignment
All USB buffers are 32-byte aligned as required by the USB controller:
```cpp
uint8_t buffer[64] __attribute__ ((aligned(32)));
```