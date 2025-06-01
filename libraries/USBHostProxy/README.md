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
- **Stored Configuration Descriptors**: Dump command uses cached descriptors, no USB communication needed
- **Force Interface Selection**: Override automatic interface selection via EEPROM configuration
- **Comprehensive Logging**: Detailed debug output on Serial4 for troubleshooting
- **Runtime Configuration**: Debug commands available via Serial4

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
- **Stores full configuration descriptor during enumeration**
- Provides control transfer API for descriptor retrieval
- Parses descriptors to find HID endpoints and interfaces
- Extracts HID descriptor lengths from HID class descriptors
- Manages interrupt endpoint data transfers
- Provides interface enumeration and query methods
- Calculates actual transfer lengths from EHCI tokens

#### HIDMouseDescriptorHandler
- Uses USBHostDriver's control transfer API to retrieve HID descriptors
- Finds and identifies mouse interfaces
- Requests HID report descriptors via USB control transfers
- Parses HID report descriptors
- Automatically falls back to boot mouse protocol
- Converts between raw HID data and MouseState structures
- **No longer sends optional SET_IDLE/SET_PROTOCOL commands**
- Supports various mouse formats
- Provides detailed descriptor analysis

#### Main Sketch (State Machine)
- Creates all USB objects as global direct instances
- Implements state machine for initialization flow
- Manages HID descriptor retrieval process
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
               ├─> Store configuration descriptor
               ├─> Extract HID descriptor lengths
               ├─> Check for force claim config
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

### 5. Stored Configuration Descriptors
The configuration descriptor is stored during enumeration, allowing the dump command to display complete device information without any USB communication.

### 6. Optional HID Commands Removed
SET_IDLE and SET_PROTOCOL commands have been removed as they are optional per the HID specification and many devices don't support them (returning STALL).

## Logging Structure

### Log Prefixes
- `[STARTUP]:` - USBHostDriver and startup messages
- `[MAIN]:` - Main sketch state machine and status
- `[HID_HANDLER]:` - HID descriptor handler messages
- `[DRIVER]:` - USB driver control transfer messages
- `[PARSER]:` - HID report parser messages
- `[DATA]:` - Data reception messages
- `[OVERRIDE]:` - Force claim configuration messages

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
[STARTUP]: Device VID: 0x046D, PID: 0xC53F
[STARTUP]: Interface level claim (type 1) - claiming device!
[STARTUP]: Stored 75 bytes of configuration descriptor data
[STARTUP]: Found interface 0, class: 0x3 (HID Keyboard)
[STARTUP]: Found interface 1, class: 0x3 (HID Mouse)
[STARTUP]: Found interface 2, class: 0x3 (HID)
[OVERRIDE]: Found forced interface configuration!
[OVERRIDE]: Using interface 1 endpoint 0x82 for device VID:0x46D PID:0xC53F
[STARTUP]: Device successfully claimed!
[MAIN]: Device detected and claimed!
[MAIN]: Setting up HID mouse interface...
[HID_HANDLER]: Found mouse interface 1 at index 1
[MAIN]: Requesting HID descriptor...
[DRIVER]: Control transfer: bmRequestType=0x81 bRequest=0x6 wValue=0x2200 wIndex=0x1 wLength=148
[DRIVER]: Control transfer complete, received 148 bytes
[HID_HANDLER]: Retrieved 148 bytes of HID descriptor
=== Parsing HID Report Descriptor ===
[PARSER]: Found: Buttons X Y Wheel
[MAIN]: HID descriptor processed!
[MAIN]: Activating HID interface...
[HID_HANDLER]: Interface activation complete
[MAIN]: HID device ready - initializing USB device stack...
[MAIN]: Proxy fully initialized and ready!
```

## Debug Interface

### Serial4 Commands (115200 baud)
- `debug` - Toggle debug mode (shows raw HID data)
- `dump` - Display stored device descriptors (no USB communication)
- `force vid,pid,interface,endpoint` - Force claim specific interface
- `clear` - Clear force claim configuration
- `status` - Show system status
- `info` - Show interface/descriptor info
- `help` or `?` - Show help

### Force Command Example
To force the system to claim a specific interface:
```
force 046d,c53f,1,82
```
This forces the driver to claim interface 1 with endpoint 0x82 for device VID=0x046D, PID=0xC53F. The configuration is stored in EEPROM and persists across reboots.

### Dump Command
The dump command displays the complete device configuration including:
- Reconstructed configuration descriptor header
- All interfaces with their class/subclass/protocol
- HID descriptor information
- All endpoints with attributes and packet sizes
- Raw hex dump of descriptors

This information is retrieved from stored descriptors captured during enumeration, requiring no USB communication.

### Status Display
```
=== System Status ===
State: PROXY_ACTIVE
Device: Connected (VID:0x46D PID:0xC53F)
HID Handler: Ready (Interface 1, EP 0x82)
HID Parser: Parsed
USB Device Stack: Initialized
Debug Mode: OFF
Force Claim: VID=0x46D PID=0xC53F Interface=1 Endpoint=0x82
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
    uint8_t buttons;  // Button states (up to 8 buttons)
    
    bool leftButton() const { return buttons & 0x01; }
    bool rightButton() const { return buttons & 0x02; }
    bool middleButton() const { return buttons & 0x04; }
    bool button4() const { return buttons & 0x08; }
    bool button5() const { return buttons & 0x10; }
    // ... up to button8
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
3. Copy all project files to sketch folder:
   - SunshineUSBProxy.ino
   - USBHostDriver.h/cpp
   - HIDMouseDescriptorHandler.h/cpp
   - SunBoxStartup.h/cpp
   - startup.c (if modifying Teensyduino core)
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
3. Device will automatically fall back to boot protocol
4. Check timeout values

### Incorrect Interface Claimed
1. Use `dump` command to see all available interfaces
2. Identify correct interface number and endpoint
3. Use `force` command to override automatic selection
4. Reboot to apply force configuration

### Control Transfer Errors
1. Token 0x40 in status = STALL (device rejection)
2. Token 0x80 = Active (transfer still in progress)
3. Some devices don't support optional HID commands (normal)

### USB Device Stack Issues
1. Verify usb_init() is called only in USB_DEVICE_READY state
2. Check that HID parsing completed successfully
3. Monitor Serial4 for initialization sequence
4. "endpoint 0 stall" errors are expected until usb.c is updated

## Technical Notes

### Control Transfer Implementation
The USBHostDriver provides a complete control transfer API that handles:
- Setup packet formation
- Data stage management
- Status stage completion
- Timeout handling
- Error detection
- Proper synchronization with interrupt transfers

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

### Configuration Descriptor Storage
The system stores the full configuration descriptor (minus the 9-byte header that enumeration.cpp strips) during the claim process. This allows complete device information to be displayed without any USB communication.

## Device Compatibility

The following devices have been tested with the SunBox USB Proxy System:

| Device | VID | PID | Status | Interfaces | Notes |
|--------|-----|-----|--------|------------|-------|
| Logitech USB Receiver | 0x046D | 0xC53F | ✅ Working | 3 (Keyboard, Mouse, Generic HID) | Force command tested successfully |
| Glorious Model O Wireless | 0x258A | 0x2022 | ✅ Working | 3 (Mouse, Keyboard, Generic HID) | 5 buttons, 16-bit X/Y |
| BenQ Zowie | 0x3662 | 0x2004 | ✅ Working | 3 (Mouse, Keyboard, Generic HID) | 5 buttons, 16-bit X/Y |
| Pwnage StormBreaker CF | 0x04A5 | 0x8001 | ✅ Working | 1 (Mouse only) | 6 buttons, 16-bit X/Y, compact format |
| Beast X Mini | 0x3662 | 0x2004 | ✅ Working | 3 (Mouse, Keyboard, Generic HID) | Same as BenQ Zowie |

### Compatibility Notes

- All tested devices support the standard HID mouse protocol
- No devices required special initialization beyond standard HID commands
- SET_IDLE and SET_PROTOCOL commands removed as they often fail with STALL
- All devices work without a powered USB hub
- Movement data uses 16-bit signed integers for precise tracking
- Button layouts are consistent across devices (bits 0-7 for up to 8 buttons)

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
   - Coordinate transformation

4. **Performance Optimization**
   - Zero-copy data paths
   - DMA transfer optimization
   - Reduced interrupt latency

5. **Enhanced Diagnostics**
   - Packet capture and replay
   - Latency measurement
   - Bandwidth utilization

## Version History

### v1.2 (Current)
- Removed optional SET_IDLE and SET_PROTOCOL commands
- Added configuration descriptor storage
- Improved dump command to use stored descriptors
- Fixed logging overlap issues
- Force command fully tested and working

### v1.1
- Added force interface selection
- Improved HID descriptor parsing
- Added boot protocol fallback

### v1.0
- Initial release
- Basic USB proxy functionality
- HID mouse support

## SunshineUSBProxy Sketch Architecture

The SunshineUSBProxy sketch implements a modular USB proxy system with intelligent command routing and multi-protocol support. This section explains how the system works.

### Overview

The proxy acts as a bridge between a USB HID mouse device and the host computer, with the ability to intercept, modify, and inject mouse data from multiple sources.

```
USB Mouse → Teensy USB Host → Processing → Teensy USB Device → Computer
              ↑                    ↑
              |                    |
         Serial Commands      Serial Mouse Data
```

### Core Components

#### 1. **SunBoxCommands** - Intelligent Router
The central routing system that reads from a single serial port (Serial4) and intelligently routes data to the appropriate handler:

- **Routing Logic**:
  - Binary data starting with 3 or 8 → Legacy Sunshine Protocol
  - Text starting with "km." → KMBox Protocol  
  - Other text commands → DevTools Interface

- **Buffer Management**: Accumulates serial data and analyzes patterns to determine routing
- **Protocol Auto-Detection**: Automatically detects which protocol is being used

#### 2. **Command Handlers**

**CommandsSunBoxDevtoolsInterface**:
- Handles system commands: `help`, `status`, `debug`, `dump`, `claimcorrection`, `claimclear`
- Manages persistent settings via EEPROM
- Provides system diagnostics and configuration

**CommandsSunBoxInterface** (Legacy Sunshine):
- Processes 9-byte length-prefixed binary protocol
- Format: `[Length][Data...]`
  - Length 8: HID mouse report
  - Length 3: Settings update
- Maintains mouse state for legacy applications

**CommandsSunBoxKMBoxInterface**:
- Processes ASCII text commands
- KMBox B+ protocol support (currently prints received commands)
- Future: Full command parsing for km.move(), km.click(), etc.

#### 3. **SunBoxUSBMouseDataHandler**
Manages USB device data:
- Monitors USB connection state
- Processes incoming HID reports via callback
- Detects state changes (button presses/releases, movement)
- Only flags new data when state actually changes

#### 4. **SunBoxSyntheticHandleOutput**
Combines data from multiple sources:
- **Mixing Modes**:
  - `USB_ONLY`: Only USB mouse data
  - `SERIAL_ONLY`: Only serial command data
  - `BOTH_REPLACE`: Serial overrides USB
  - `BOTH_ADD`: Movements are added together
- Outputs combined mouse state
- Tracks changes to prevent duplicate outputs

#### 5. **SunBoxEEPROM**
Persistent storage management:
- Debug mode state (survives power cycles)
- Claim correction configuration
- Structured storage with magic numbers for validation

### Data Flow

1. **USB Mouse Input**:
   ```
   Physical Mouse → USB Host → USBHostDriver → HIDMouseDescriptorHandler
   → SunBoxUSBMouseDataHandler → MouseState
   ```

2. **Serial Input**:
   ```
   Serial Port → SunBoxCommands → Protocol Detection → Appropriate Handler
   → MouseState or System Command
   ```

3. **Output Processing**:
   ```
   if (USB data available OR Serial data available)
       → SunBoxSyntheticHandleOutput.process()
       → Mix data based on mode
       → Output combined MouseState
   ```

### Initialization Sequence

1. **Early Boot** (startup.c):
   - Hardware initialization
   - Serial4 setup for debugging
   - **Does NOT call usb_init()** yet

2. **Setup Phase**:
   - Create all objects as global instances
   - Initialize EEPROM and load saved settings
   - Start USB Host system
   - Initialize all components

3. **Runtime State Machine**:
   - Wait for USB device connection
   - Detect and claim HID mouse interface
   - Retrieve and parse HID descriptors
   - **Only NOW initialize USB device stack**
   - Enter active proxy mode

### Protocol Details

#### Legacy Sunshine Protocol (Binary)
```
[Length: 1 byte][Data: Length bytes][Padding to 9 bytes total]

HID Report (Length = 8):
  Byte 0: Buttons
  Bytes 1-2: X movement (16-bit, format TBD)
  Byte 3: Wheel
  Bytes 4-5: Y movement (16-bit, format TBD)
  Bytes 6-7: Reserved

Settings (Length = 3):
  Byte 0: Sensitivity
  Byte 1: Acceleration  
  Byte 2: Smoothing
```

#### KMBox Protocol (ASCII)
```
km.move(x,y,wheel)   - Relative mouse movement
km.click(button)     - Click and release
km.press(button)     - Press and hold
km.release(button)   - Release button
```

#### DevTools Commands
```
help              - Show available commands
status            - System status display
debug             - Toggle debug mode (persistent)
dump              - Display USB descriptors
claimcorrection   - Force specific interface/endpoint
claimclear        - Clear forced configuration
```

### Key Features

1. **Single Serial Port**: All commands and data go through Serial4, with intelligent routing based on content

2. **State Change Detection**: Only processes data when something actually changes (buttons, movement, wheel)

3. **Persistent Configuration**: Debug mode and claim corrections survive power cycles via EEPROM

4. **Flexible Data Mixing**: Multiple modes for combining USB and serial mouse data

5. **Clean Architecture**: Each component has a single responsibility, making the system maintainable and extensible

### Extending the System

To add new functionality:

1. **New Serial Protocol**: Add detection logic to `SunBoxCommands::detectAndRoute()`
2. **New Commands**: Add to `CommandsSunBoxDevtoolsInterface`
3. **New Mouse Features**: Extend `MouseState` structure and parsing logic
4. **New Output Modes**: Add to `SunBoxSyntheticHandleOutput` mixing logic

### Debugging

Enable debug mode to see:
- Mouse state changes
- Protocol detection
- USB device enumeration
- Data routing decisions

Debug mode is persistent across power cycles when toggled via the `debug` command.