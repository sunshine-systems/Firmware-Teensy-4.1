# USB HID Proxy for Teensy 4.1

## Overview

This project implements a transparent USB HID proxy using the Teensy 4.1's dual USB ports. The proxy intercepts and forwards USB communications between a Windows host and a USB HID device (mouse/keyboard) while maintaining full functionality and timing requirements.

### Key Features
- **Transparent Proxying**: Device appears identical to Windows
- **Real-time HID Data Forwarding**: Mouse movements and keyboard inputs forwarded with minimal latency
- **Full HID Control Transfer Support**: SET_REPORT and GET_REPORT for device configuration
- **Async Control Transfer Handling**: Non-blocking design that respects USB timing
- **Multi-Interface Support**: Handles composite devices with multiple HID interfaces
- **Fire-and-Forget Architecture**: Leverages USB's built-in NAK/retry mechanism
- **Universal Speed Support**: Works with Low Speed (1.5 Mbps), Full Speed (12 Mbps), and High Speed (480 Mbps) devices
- **Race Condition Prevention**: Immediate USB initialization with proper request queuing

## Architecture

```
Windows Host <--USB Device--> Teensy 4.1 <--USB Host--> HID Device
                               |        |
                               |        └── USBHost_t36 (captures from device)
                               └── USB Device Stack (presents to Windows)
```

### Components

1. **USB Device Stack** (`usb.c`)
   - Modified Teensy core USB implementation
   - Intercepts control transfers in `endpoint0_setup()`
   - Forwards requests asynchronously
   - Injects responses when ready
   - Handles HID SET_REPORT/GET_REPORT requests
   - **NAKs early requests until proxy is ready**

2. **USB Host Stack** (`USBHost_t36`)
   - `USBProxyDriver`: Claims all HID interfaces
   - `ForwardControlHelper`: Manages control transfer forwarding
   - Real-time HID data capture and forwarding
   - Properly handles actual transfer lengths for all USB speeds

3. **Async Forwarding** (`async_forwarder.cpp`)
   - Queues control transfers without blocking
   - Manages state between ISR contexts
   - Handles completion callbacks
   - **Properly clears all state on timeouts**

4. **State Management** (`passthrough_state.c`)
   - Tracks pending requests
   - Stores response data
   - Manages injection flags
   - **Maintains request_pending flag for race condition prevention**

## How It Works

### 1. Initialization Sequence (Race Condition Fix)

The proxy uses a carefully orchestrated initialization sequence to prevent race conditions:

```c
// State machine in main loop
case DEVICE_READY:
    // Initialize USB device immediately - no delay!
    g_passthrough_ready = 1;
    usb_init();
    g_need_passthrough_init = 0;
    
    // Very short delay for USB stack to stabilize
    delayMicroseconds(100);
    
    currentState = PASSTHROUGH_ACTIVE;
```

**Key Insight**: Windows starts enumeration immediately when it detects a device. By initializing the USB device stack without delay, we ensure it's ready to NAK requests while the proxy establishes forwarding.

### 2. Control Transfer Forwarding with Early Request Handling

The proxy uses a **NAK/retry mechanism** to handle control transfers asynchronously:

```c
// In ISR context (endpoint0_setup)
if (g_passthrough_ready) {
    // Check if we're already processing a request
    if (g_passthrough_state.request_pending) {
        printf("[USB] Request pending, NAKing (not stalling)\n");
        return; // NAK - tells Windows "try again later"
    }
    
    // Start async forward
    if (passthrough_start_request(&setup)) {
        if (start_async_forward(&setup)) {
            return; // NAK - Windows will retry
        }
    }
}
```

**Key Insight**: NAKing (not stalling) allows Windows to retry gracefully. This prevents enumeration failures when requests arrive before the proxy is ready.

### 3. Response Injection

When the forwarded request completes:

```c
// Next time Windows retries
if (response_ready) {
    endpoint0_transmit(response_data, response_len, 0);
    clear_injection_flag();
}
```

### 4. Timeout Handling with Proper State Clearing

```c
void process_async_transfers(void) {
    if (millis() - g_passthrough_state.request_timestamp > 100) {
        printf("[USB] Async transfer timeout - clearing state\n");
        
        // Clear ALL state properly
        passthrough_timeout_response();
        async_busy = false;
        
        // Clear the pending flag too
        g_passthrough_state.request_pending = 0;
    }
}
```

**Key Insight**: Clearing all state flags on timeout prevents the proxy from getting stuck in a "request pending" state.

### 5. HID Data Forwarding

HID interrupt endpoints forward data in real-time:

```c
void USBProxyDriver::in_data(const Transfer_t *transfer) {
    // Calculate actual bytes transferred (EHCI protocol)
    uint32_t token = transfer->qtd.token;
    uint32_t bytes_not_transferred = (token >> 16) & 0x7FFF;
    uint32_t actual_len = transfer->length - bytes_not_transferred;
    
    // Forward HID report to host
    usb_hid_send_report(endpoint, data, actual_len);
    
    // Re-queue for continuous data flow
    queue_Data_Transfer(pipe, buffer, max_size, this);
}
```

### 6. HID Control Transfers (SET_REPORT/GET_REPORT)

Many HID devices, especially gaming peripherals, use control transfers for configuration:

#### SET_REPORT (Host sends data to device)
1. Host sends SET_REPORT request with data length
2. Proxy allocates buffer space and receives data
3. Data is forwarded to the actual device
4. Device acknowledgment is passed back to host

#### GET_REPORT (Host requests data from device)
1. Host sends GET_REPORT request
2. Proxy forwards request to device
3. Device responds with report data
4. Proxy injects response back to host

**Buffer Management**: The proxy uses a 256-byte buffer for control transfers, which accommodates all common HID report sizes:
- Basic mice/keyboards: 8 bytes
- Gaming devices: 64 bytes  
- Complex devices: up to 256 bytes

The actual transfer size is always exact - a 64-byte report uses only 64 bytes, regardless of buffer size.

## Critical Design Decisions

### 1. Non-Blocking ISR Design

**Problem**: USB ISRs cannot be blocked without breaking the USB stack.

**Solution**: Fire-and-forget design that never waits:
- Queue requests and return immediately
- Use volatile flags for ISR communication
- Let USB's NAK/retry handle timing

### 2. Race Condition Prevention

**Problem**: Windows sends enumeration requests before proxy forwarding is ready.

**Solution**: 
- Initialize USB device immediately when host device is detected
- NAK (don't STALL) requests while proxy sets up forwarding
- Maintain proper state flags to track pending requests

### 3. State Recovery on Timeout

**Problem**: Timeout could leave proxy in inconsistent state.

**Solution**: Clear all state flags when timeout occurs:
- Clear `async_busy`
- Clear `request_pending`
- Send timeout response to trigger Windows retry

### 4. Dynamic Buffer Sizing

**Problem**: Different HID devices use different report sizes (8, 64, 256 bytes).

**Solution**: Use a 256-byte buffer that can handle any size:
```c
static uint8_t endpoint0_buffer[256] __attribute__ ((aligned(32)));

// Receive exactly the requested amount
endpoint0_receive(endpoint0_buffer, setup.wLength, 1);

// Send exactly the response length
endpoint0_transmit(response_buffer, actual_length, 0);
```

### 5. String Descriptor Length Fix

**Problem**: String descriptors were being rejected by Windows.

**Solution**: Use actual descriptor length from first byte:
```c
if (desc_type == 0x03 && len > 0) { // String descriptor
    uint8_t actual_len = response_buffer[0];
    if (actual_len < len) {
        len = actual_len; // Use real length, not requested
    }
}
```

### 6. Endpoint Configuration

**Problem**: Windows expects specific endpoint configurations.

**Solution**: Configure Teensy endpoints to match device:
```c
// Map device endpoints directly
iface->teensy_in_endpoint[j] = device_endpoint_num;
```

### 7. Actual Transfer Length Calculation

**Problem**: USBHost_t36 reports buffer size instead of actual bytes transferred for interrupt endpoints.

**Solution**: Calculate actual transfer length from EHCI qTD token field:
```c
// EHCI spec: Actual bytes = Requested - Not transferred
uint32_t token = transfer->qtd.token;
uint32_t bytes_not_transferred = (token >> 16) & 0x7FFF;
uint32_t actual_len = transfer->length - bytes_not_transferred;
```

## Implementation Details

### USB Timing

- **Initial NAKs**: Windows retries quickly (microseconds)
- **Backoff**: After ~5-6 NAKs, Windows backs off to 5-6 second intervals
- **Typical Response Time**: ~28ms for control transfers
- **HID Latency**: < 1ms for interrupt transfers
- **Initialization**: < 100μs from device detection to USB ready

### Memory Requirements

- **Aligned Buffers**: All USB buffers must be 32-byte aligned
- **Transfer Structures**: Pre-allocated for USB Host operations
- **Response Buffer**: 512 bytes for control transfer responses
- **EP0 Buffer**: 256 bytes for HID control transfers

### Special Handling

1. **SET_ADDRESS**: Always handled locally (required for USB hardware)
2. **SET_CONFIGURATION**: Configured locally AND forwarded
3. **SET_REPORT**: Two-stage process (receive data, then forward)
4. **GET_REPORT**: Forward request, inject response
5. **String Descriptors**: Length-corrected before transmission

### HID Report Types

The proxy handles all three HID report types:

1. **Input Reports** (Device to Host)
   - Sent via interrupt endpoints (mouse/keyboard data)
   - Forwarded in real-time with minimal latency

2. **Output Reports** (Host to Device)
   - LED states, rumble commands
   - Can be sent via interrupt OUT or SET_REPORT

3. **Feature Reports** (Bidirectional)
   - Configuration data
   - Accessed via SET_REPORT/GET_REPORT
   - Used by gaming software for RGB, DPI, etc.

### EHCI Transfer Length Details

The EHCI (Enhanced Host Controller Interface) specification defines how to determine actual transfer lengths:

- **qTD Token Field** (32 bits):
  - Bits 31: Data Toggle
  - Bits 30-16: Total Bytes to Transfer (decrements during transfer)
  - Bit 15: Interrupt On Complete (IOC)
  - Bits 7-0: Status

For completed transfers, the "Total Bytes to Transfer" field contains the bytes NOT transferred, allowing accurate calculation of actual data length regardless of buffer size.

## Race Condition Deep Dive

### The Problem

The original implementation had a 100ms delay between device detection and USB initialization:

```
1. USB Host detects device and enumerates it
2. State changes to DEVICE_READY
3. 100ms delay
4. USB device stack initializes
5. Windows sends first request during the delay
6. No USB stack to handle it → timeout → STALL
```

### The Solution

Immediate initialization with proper request handling:

```
1. USB Host detects device
2. State changes to DEVICE_READY
3. USB device stack initializes immediately
4. Windows sends first request
5. Proxy NAKs while setting up forwarding
6. Windows retries, proxy responds when ready
```

### Why This Works

- **NAK vs STALL**: NAK means "not ready, try again" while STALL means "error"
- **Windows Retry**: Windows automatically retries NAKed requests
- **State Management**: Proper flags prevent request collisions
- **Timeout Recovery**: All state cleared on timeout to prevent stuck states

## Supported Devices

Tested and working with:
- **High Speed (480 Mbps)**: Pwnage Ultra Custom Wireless Mouse (64-byte feature reports)
- **Full Speed (12 Mbps)**: Glorious Model O Mouse, Custom Arduino HID devices
- **Low Speed (1.5 Mbps)**: Legacy keyboards and mice (8-byte reports)

The proxy correctly handles:
- Different report sizes (8, 64, 256 bytes)
- Vendor-specific control transfers
- Gaming software configuration utilities
- RGB control software
- DPI adjustment tools

## Limitations

1. **Control Transfer Size**: Maximum 256 bytes (can be increased if needed)
2. **Single Device**: One device at a time
3. **HID Only**: Designed specifically for HID devices

## Building and Usage

### Requirements
- Teensy 4.1 (dual USB ports required)
- PlatformIO or Arduino IDE with Teensyduino
- USBHost_t36 library (with our modifications for HS support)

### Installation
1. Clone the repository
2. Open in PlatformIO or Arduino IDE
3. Build and upload to Teensy 4.1
4. Connect HID device to USB Host port
5. Connect Teensy to Windows via USB Device port

### Debug Output
- Serial1 (hardware serial) outputs debug information
- View with serial monitor at 115200 baud
- Press 'd' to toggle detailed HID data logging

### Serial Commands
- `d` - Toggle debug mode
- `s` - Show detailed status
- `x` - Execute HID test sequence (SET_REPORT/GET_REPORT)
- `r` - Reset HID statistics

## Technical Insights

### Why NAK/Retry Works

USB's NAK mechanism is designed for exactly this scenario:
- Devices can NAK indefinitely for control transfers
- Hosts must retry NAKed requests
- No timeout specified in USB spec for control transfers
- NAK doesn't indicate error, just temporary unavailability

### Interrupt Context Challenges

Working in ISR context requires:
- No blocking operations (no delays, waits, or polling)
- Minimal processing time
- Communication via volatile flags
- Careful state management
- Atomic state transitions

### Windows USB Stack Behavior

- Aggressive initial retry (good for fast devices)
- Exponential backoff (prevents bus flooding)
- Enumeration continues despite NAKs
- String descriptors cached by Windows after first read
- STALL responses can abort enumeration

### HID Control Transfer Flow

For SET_REPORT:
```
1. Host → Proxy: SET_REPORT request (setup packet)
2. Host → Proxy: Report data (data packet)
3. Proxy → Device: Forwarded SET_REPORT + data
4. Device → Proxy: Acknowledgment
5. Proxy → Host: Acknowledgment
```

For GET_REPORT:
```
1. Host → Proxy: GET_REPORT request
2. Proxy → Device: Forwarded GET_REPORT
3. Device → Proxy: Report data
4. Proxy → Host: Report data
```

## Troubleshooting

### Mouse/Keyboard Not Working
1. Enable debug mode (press 'd' in serial monitor)
2. Check that HID data is being received and forwarded
3. Verify endpoint mappings match the device
4. Ensure actual transfer lengths are correct (not buffer sizes)

### Device Not Enumerating
1. Check USB connections
2. Verify device is detected by USB Host
3. Monitor control transfer forwarding
4. Check for stalled requests in debug output
5. Look for "Request pending, NAKing" messages

### Device Enumeration Stalls
1. Check for "STALL" in the logs
2. Verify no 100ms delays in initialization
3. Ensure state is properly cleared on timeouts
4. Look for "Already processing a request" errors

### Gaming Software Not Detecting Device
1. Check SET_REPORT/GET_REPORT are working
2. Verify 64-byte feature reports are handled
3. Look for vendor-specific control transfers
4. Ensure report data matches expected format

## Future Enhancements

1. **Multiple Devices**: Support USB hub functionality
2. **Bulk Transfer Support**: Extend to mass storage devices
3. **Data Modification**: Add hooks for modifying HID reports
4. **Protocol Analysis**: Log and analyze USB traffic
5. **Larger Buffer Support**: Dynamic allocation for >256 byte reports
6. **Selective NAK**: NAK only specific request types during init

## Technical References

- [USB 2.0 Specification](https://www.usb.org/document-library/usb-20-specification)
- [EHCI Specification](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/ehci-specification-for-usb.pdf)
- [HID 1.11 Specification](https://www.usb.org/document-library/device-class-definition-hid-111)
- [USB in a NutShell](https://www.beyondlogic.org/usbnutshell/usb1.shtml)

## Credits

Built using:
- [Teensy 4.1](https://www.pjrc.com/store/teensy41.html) by PJRC
- [USBHost_t36](https://github.com/PaulStoffregen/USBHost_t36) library (modified for HS support)
- Modified Teensy Core USB stack

## License

MIT License - See LICENSE file for details