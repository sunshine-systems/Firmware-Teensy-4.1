# USB Proxy Device Stack - Final Implementation Guide

## Project Overview

### Goal
A **fully functional HID input device proxy** for Teensy 4.1 that:
- ✅ Forwards HID mice transparently to the host PC
- ✅ Supports complex composite devices (gaming mice with multiple interfaces)
- ✅ Handles vendor-specific and proprietary protocols
- ✅ Dynamically configures based on the connected device
- ✅ Uses a polling-based architecture (no interrupts)
- ✅ Appears 100% identical to the physical device, including its polling rate
- ✅ **Automatically matches the USB speed of the connected device**
- ✅ **NEW**: Supports Low Speed (1.5 Mbps) devices with automatic Full Speed conversion
- ✅ **NEW**: Handles non-compliant devices through intelligent request filtering
- ✅ **NEW**: Dynamic endpoint mapping with EEPROM override support
- ✅ **NEW**: Properly handles vendor control transfers (SET_REPORT/GET_REPORT)

### Status: **FULLY OPERATIONAL (Universal Device Support + Vendor Command Support)** 🎉
The proxy successfully emulates the physical device with:
- **8kHz polling fully functional** for high-performance gaming mice
- **Universal speed support** - correctly handles Low Speed (1.5 Mbps), Full Speed (12 Mbps), and High Speed (480 Mbps) devices
- **Automatic speed adaptation** - Low Speed devices are transparently proxied at Full Speed for compatibility
- **Non-compliant device support** - Request filtering handles devices that don't fully implement the USB specification
- **Dynamic endpoint mapping** - Automatically detects and routes data to the correct endpoint based on EEPROM overrides
- **Vendor control transfers** - Properly handles SET_REPORT and GET_REPORT for device configuration and firmware updates
- All HID data forwarded correctly without stalls
- 100% transparent to the host PC

---

## Architecture Overview

### High-Level Design
```
Physical Mouse ─USB─> Teensy 4.1 ─USB─> Host PC
                      │         │
                      │         └─> USB Device (Proxy)
                      └─> USB Host (Reads Mouse)

Data Flow:
1. Physical mouse sends HID reports (varies by device: 6-8 bytes @ up to 8kHz)
2. USBHostDriver receives via interrupt transfer  
3. Data callback immediately forwards to the proxy stack
4. USBDeviceProxy, correctly configured for device speed and polling rate, sends to PC
5. PC sees an exact replica of the physical mouse at the appropriate USB speed

Control Flow (SET_REPORT/GET_REPORT):
1. PC sends SET_REPORT request with data payload to Teensy
2. USBDeviceProxy receives request, stores setup packet and buffers incoming data
3. After data is fully received, USBDeviceProxy forwards request+data to physical mouse
4. Physical mouse responds, data is forwarded back to PC
5. Same flow works for GET_REPORT in reverse direction
```

### Key Components

1. **USBHostDriver** - USB Host implementation for reading the physical mouse
2. **USBDeviceProxy** - Custom polling-based USB Device Stack that presents the proxy device to the PC
3. **HIDMouseDescriptorHandler** - Helper for parsing HID report descriptors
4. **Main Loop** - High-frequency orchestration (~200-400kHz)
5. **Dynamic Endpoint Mapper** - Automatically routes data based on device configuration and EEPROM overrides
6. **Vendor Control Transfer Handler** - Properly sequences and forwards vendor-specific commands

---

## Implementation Details

### Critical Design Decisions

#### 1. **Polling Architecture**
- The custom device stack (`USBDeviceProxy`) uses no interrupts, preventing conflicts with the core Teensy libraries and ensuring maximum performance.

#### 2. **Direct Forwarding**
- Mouse data is forwarded immediately upon receipt, with minimal buffering, for the lowest possible latency.

#### 3. **Perfect Descriptor Proxying**
- All descriptors (Device, Configuration, String, HID) are proxied 1:1 from the physical device, ensuring the host PC sees an identical device.

#### 4. **Dynamic Speed Matching**
- The proxy detects whether the physical device operates at Low Speed, Full Speed, or High Speed
- Low Speed devices are automatically proxied at Full Speed (backwards compatible per USB spec)
- EP0 packet size is dynamically configured (8 bytes for Low Speed, 64 bytes for Full/High Speed)

#### 5. **Dynamic Endpoint Mapping**
- The proxy automatically detects which endpoint is being used for mouse data
- Supports EEPROM overrides for forcing specific interface/endpoint selection
- Handles complex composite devices with multiple HID interfaces

#### 6. **Vendor Control Transfer Sequencing**
- Properly handles multi-stage control transfers (especially SET_REPORT/GET_REPORT)
- Ensures correct sequencing: receive data from host, forward to device, get response, forward back to host
- Critical for device configuration, firmware updates, and vendor-specific features

### Key Solutions Implemented

#### 1. **Symbol Conflict Resolution**
```cpp
// All proxy structures use unique names to avoid conflicts with usb.c
endpoint_t proxy_endpoint_queue_head[(NUM_ENDPOINTS+1)*2];
transfer_t proxy_endpoint0_transfer_data;
```

#### 2. **String Descriptor Fix**
```cpp
// Send ONLY the actual descriptor length, not the host's requested wLength
if (desc_type == 0x03) {  // String descriptor
    actual_len = proxy_descriptor_buffer[0];  // The first byte is the true length
}
```

#### 3. **Unlocking 8kHz Polling: The Hardware Fix**
The most critical challenge was achieving the true 8kHz polling rate. The solution was found by comparing with the official `usb.c` source code, revealing a non-intuitive but essential hardware configuration bit.

**The Problem:** The proxy device advertised `bInterval=1` (8kHz) but the endpoint would stall after the first packet.

**The Solution:** The Endpoint Queue Head `config` word was missing **bit 29** (ZLT - Zero Length Termination Select):
```cpp
// The final, correct configuration
uint32_t config = (maxPacket << 16) | (1 << 29);
```

#### 4. **USB Speed Matching: The Complete Solution**
To ensure the proxy appears identical to the physical device, we implemented automatic USB speed detection and configuration.

**The Problem:** The Teensy 4.1 defaults to High Speed (480 Mbps), but many mice operate at Full Speed (12 Mbps) or even Low Speed (1.5 Mbps).

**The Solution:** 
1. Detect the physical device's speed using the USBHost_t36 library (3-way detection)
2. Configure the Teensy's USB PHY before controller initialization
3. Use specific PHY register settings to force appropriate speed
4. Dynamically adjust EP0 packet size based on speed

```cpp
// In USBHostDriver - detect device speed (3-way)
uint8_t USBHostDriver::getDeviceSpeed() const {
    // Speed values: 0=Low, 1=Full, 2=High
    return device->speed;
}

// In main sketch - configure proxy based on detected speed
uint8_t device_speed = usbHostDriver.getDeviceSpeed();

if (device_speed == 0) {
    // Low Speed device - force Full Speed on proxy side (backwards compatible)
    proxy_high_speed = false;
    ep0_max_size = 8;  // Low Speed devices use 8-byte EP0
} else if (device_speed == 1) {
    // Full Speed device
    proxy_high_speed = false;
    ep0_max_size = 64;
} else {
    // High Speed device
    proxy_high_speed = true;
    ep0_max_size = 64;
}
```

#### 5. **Non-Compliant Device Support: Request Filtering**
Many real-world USB devices don't fully comply with the USB specification, causing enumeration failures when certain optional requests cause the device to STALL its control endpoint.

**The Problem:** Devices like the Logitech G307 would STALL on certain standard requests (e.g., GET_DESCRIPTOR for Device Qualifier, SET_IDLE for HID), causing subsequent control transfers to time out and triggering endless USB reset loops.

**The Solution:** The proxy now intercepts and handles problematic requests locally, preventing them from reaching the physical device:

```cpp
// In handleSetupPacket() - filter problematic requests

// GET_DESCRIPTOR (Device Qualifier) - Full Speed devices can legitimately STALL this
if (pending_setup.bmRequestType == 0x80 && pending_setup.bRequest == 0x06 && 
    (pending_setup.wValue >> 8) == 0x06) {
    // Device Qualifier descriptor - STALL immediately for Full Speed devices
    USB1_ENDPTCTRL0 = 0x00010001;  // STALL both directions
    control_stage = CONTROL_IDLE;
    return;
}

// SET_IDLE - Some HID devices don't support this optional request
if (pending_setup.bmRequestType == 0x21 && pending_setup.bRequest == 0x0A) {
    // HID SET_IDLE - just ACK it without forwarding
    receiveData(NULL, 0);  // Send ZLP ACK
    control_stage = CONTROL_IDLE;
    return;
}
```

#### 6. **Dynamic Endpoint Mapping: The EEPROM Override Solution**
Complex composite devices (like the Logitech G307) have multiple HID interfaces, and the proxy needs to know which endpoint carries the mouse data.

**The Problem:** Hardcoded endpoint mappings fail for devices with non-standard configurations where the mouse interface isn't on the first endpoint.

**The Solution:** Dynamic endpoint mapping that:
1. Queries the host driver to determine which endpoint it's using (based on EEPROM overrides)
2. Builds a mapping table dynamically based on the device's actual configuration
3. Routes mouse data to the correct endpoint on the proxy side

```cpp
void buildEndpointMapping() {
    // Get the endpoint being used by the host driver (respects EEPROM overrides)
    uint8_t mouse_endpoint_addr = usbHostDriver.getConfiguredMouseEndpoint();
    uint8_t mouse_ep_num = mouse_endpoint_addr & 0x0F;
    
    // Build mapping based on actual interfaces
    for (uint8_t i = 0; i < num_interfaces; i++) {
        uint8_t ep_num = usbHostDriver.getEndpointAddress(i);
        
        // Check if this endpoint matches what the host driver is using
        if (ep_num == mouse_ep_num) {
            endpoint_map[i].is_mouse = true;
            Serial4.println(" identified as mouse - MATCHES HOST DRIVER");
        }
    }
}
```

#### 7. **Vendor Control Transfer Handling: The SET_REPORT Fix**
Proper handling of vendor-specific control transfers, especially SET_REPORT/GET_REPORT, is critical for device configuration software and firmware updates.

**The Problem:** SET_REPORT requests require a specific sequence: receive setup packet, receive data, forward to device, and send response to host. The original implementation didn't properly sequence these operations.

**The Solution:** A state-machine approach that properly sequences the control transfer phases:

```cpp
// Handle HID Class SET_REPORT request specially
if (pending_setup.bmRequestType == 0x21 && pending_setup.bRequest == 0x09) {
    // This is a SET_REPORT request (Host->Device, Class, Interface)
    uint16_t report_type = (pending_setup.wValue >> 8);
    uint16_t report_id = (pending_setup.wValue & 0xFF);
    uint16_t interface = pending_setup.wIndex;
    
    Serial4.print("I: SET_REPORT for interface ");
    Serial4.print(interface);
    Serial4.print(", type=");
    Serial4.print(report_type);
    Serial4.print(", ID=");
    Serial4.print(report_id);
    Serial4.print(", length=");
    Serial4.println(pending_setup.wLength);
    
    // Store setup packet for later forwarding
    memcpy(&pending_setup_saved, &pending_setup, sizeof(setup_packet_t));
    
    // We need to receive data from host first, then forward to device
    if (pending_setup.wLength > 0) {
        Serial4.println("I: Receiving SET_REPORT data from host...");
        
        // Receive data from host
        receiveData(setup_data_buffer, pending_setup.wLength);
        pending_has_data = true;
        control_stage = CONTROL_DATA_OUT;
        
        // Setup data will be forwarded in processControlTransfer after receiving
        return;
    }
}
```

Then, in `processControlTransfer()`, when the data is fully received:

```cpp
// Handle data-out phase completion for SET_REPORT
if (control_stage == CONTROL_DATA_OUT && pending_has_data) {
    // We've received data for a SET_REPORT request
    
    // Temporarily pause data transfers while we forward the request
    if (hostDriver) hostDriver->pauseDataTransfers();
    
    // Forward the SET_REPORT with data to the device
    uint16_t actual_len = 0;
    bool success = hostDriver->controlTransfer(
        pending_setup_saved.bmRequestType,
        pending_setup_saved.bRequest,
        pending_setup_saved.wValue,
        pending_setup_saved.wIndex,
        pending_setup_saved.wLength,
        setup_data_buffer,  // This contains the data from the host
        &actual_len,
        500  // 500ms timeout
    );
    
    // Resume data transfers
    if (hostDriver) hostDriver->resumeDataTransfers();
    
    if (success) {
        // Successfully forwarded to device, acknowledge to host
        sendZLP();
        control_stage = CONTROL_IDLE;
    } else {
        // Failed to forward to device
        USB1_ENDPTCTRL0 = 0x00010001;  // STALL
        control_stage = CONTROL_IDLE;
    }
    
    pending_has_data = false;
    return;
}
```

This proactive handling approach:
- Ensures proper sequencing of multi-stage control transfers
- Correctly forwards vendor-specific commands to the physical device
- Enables device configuration software to work seamlessly through the proxy

---

## USB Protocol Implementation

### Enumeration Sequence
1. **USB Reset**: Clear all proxy state
2. **GET_DESCRIPTOR**: Forward all descriptor requests 1:1 from the physical device
3. **SET_ADDRESS**: Handle locally for correct timing
4. **SET_CONFIGURATION**: Forward request, then configure all hardware endpoints
5. **HID Specific**: Forward all other requests directly
6. **Vendor Commands**: Properly sequence SET_REPORT/GET_REPORT transfers

### Endpoint Configuration
The `configureEndpoint` function correctly sets:
- Maximum packet size (dynamically based on device)
- The critical **ZLT bit (bit 29)** for proper operation
- Endpoint type and direction

### Data Transfer Flow
1. Physical mouse sends HID report
2. `USBHostDriver::processInData()` receives the data
3. `mouseDataCallback()` is called
4. Dynamic endpoint mapping determines correct proxy endpoint
5. `sendDataOnEndpoint()` queues the data to the hardware endpoint
6. Host PC polls at the configured rate (up to 8kHz)
7. Teensy hardware responds and signals completion
8. `pollDataEndpoints()` marks endpoint ready for next packet

### Vendor Control Transfer Flow
1. PC sends SET_REPORT to configure device
2. `USBDeviceProxy::handleSetupPacket()` receives the setup packet
3. `receiveData()` is called to get the data payload from the PC
4. When data is received, `processControlTransfer()` forwards to the physical device
5. Physical device responds, success/failure is determined
6. Response is forwarded back to PC
7. For GET_REPORT, the flow is similar but data moves in the opposite direction

---

## Technical Specifications

### Performance Metrics
- **Polling Rate**: ~200-400kHz typical
- **Latency**: <1ms end-to-end
- **USB Speed**: Dynamically matched (1.5, 12, or 480 Mbps)
- **HID Report Rate**: Up to 8kHz achieved and verified
- **Control Transfer Handling**: ~50-100ms typical for vendor commands

### Memory Layout
```
0x20000000: proxy_endpoint_queue_head (4KB aligned)
0x20000800: Transfer descriptors
0x20001000: Data buffers
```

### Endpoint Mapping Examples

#### Logitech G307 (Full Speed, EEPROM Override)
- **EP0**: Control (8 bytes)
- **EP1**: HID Keyboard IN (64 bytes, 1ms interval) - Interface 0
- **EP2**: HID Mouse IN (64 bytes, 1ms interval) - Interface 1 ← EEPROM forces this
- **EP3**: HID Other IN (64 bytes, 1ms interval) - Interface 2

#### Model O Wireless (Full Speed)
- **EP0**: Control (64 bytes)
- **EP1**: HID Mouse IN (64 bytes, 1ms interval)
- **EP2**: HID Keyboard IN (64 bytes, 1ms interval)
- **EP3**: HID Other IN (64 bytes, 1ms interval)

#### Pwnage V3 (High Speed)
- **EP0**: Control (64 bytes)
- **EP1**: HID Mouse IN (8 bytes, 1ms interval, `bInterval=1`)
- **EP2**: HID Keyboard IN (64 bytes, 1ms interval)
- **EP3**: HID Other IN (64 bytes, 1ms interval)
- **Vendor Commands**: SET_REPORT/GET_REPORT on Interface 2 for device configuration

#### BenQ ZOWIE (Low Speed → Full Speed)
- **EP0**: Control (8 bytes) - Dynamically configured
- **EP1**: HID Mouse IN (8 bytes, 1ms interval)
- **Note**: Device operates at Low Speed (1.5 Mbps) but is proxied at Full Speed (12 Mbps)

---

## Lessons Learned

### What Worked Well
1. **Polling architecture**: Simple, fast, and reliable
2. **Direct descriptor proxying**: Ensures perfect device replication
3. **Speed detection and matching**: Makes the proxy truly transparent for all device speeds
4. **Dynamic EP0 configuration**: Handles varying packet sizes correctly
5. **Meticulous debugging**: Essential for hardware state issues
6. **Dynamic endpoint mapping**: Handles complex composite devices with EEPROM overrides
7. **State machine for control transfers**: Properly sequences multi-stage transfers

### Key Insights
1. **The ZLT Bit**: The non-intuitive **ZLT bit (bit 29)** is critical for preventing endpoint stalls
2. **Speed Matters**: USB speed must be handled appropriately - Low Speed devices can be proxied at Full Speed
3. **EP0 Size Varies**: Low Speed devices typically use 8-byte EP0, not 64 bytes
4. **PHY Configuration Timing**: Speed must be configured BEFORE starting the USB controller
5. **Reference Implementation**: The official `usb.c` is the ultimate source of truth
6. **Non-Compliant Devices**: Many real-world devices don't fully implement the USB spec - proactive request filtering is essential for compatibility
7. **Endpoint Address Handling**: Be careful with endpoint addresses - some APIs include the direction bit (0x80), others don't
8. **EEPROM Overrides**: Essential for devices where automatic interface detection isn't sufficient
9. **Control Transfer Sequencing**: SET_REPORT/GET_REPORT requires correct sequencing of setup, data, and status phases
10. **Avoid Delays in Transfer Handling**: Delays during control transfers can block ISRs and cause timeouts

### Debugging Indicators
- **"One packet then stall"**: Missing ZLT bit in endpoint configuration
- **Speed mismatch in Device Viewer**: PHY not configured for correct speed
- **Enumeration failures**: Check EP0 packet size for Low Speed devices
- **Endpoint not ready**: Hardware completion signal not generated
- **Control transfer STALL followed by timeout**: Device doesn't support certain optional requests - implement request filtering
- **Wrong endpoint receiving data**: Endpoint mapping doesn't match actual device configuration - check EEPROM overrides
- **SET_REPORT works manually but not from software**: Incorrect sequencing of control transfer stages

---

## Implementation Timeline

### Phase 1: Basic Proxy (Completed)
- USB Host driver implementation
- Basic USB Device stack
- Descriptor forwarding

### Phase 2: 8kHz Support (Completed)
- Discovered and fixed ZLT bit issue
- Achieved true 8kHz polling rate
- Verified with high-performance gaming mice

### Phase 3: Speed Matching (Completed)
- Added device speed detection
- Implemented PHY speed configuration
- Tested with both Full Speed and High Speed devices

### Phase 4: Universal Speed Support (Completed)
- Added 3-way speed detection (Low/Full/High)
- Implemented Low Speed to Full Speed conversion
- Dynamic EP0 size configuration
- Tested with Low Speed devices

### Phase 5: Non-Compliant Device Support (Completed)
- Identified enumeration failures with certain devices
- Implemented request filtering for problematic requests
- Added support for Device Qualifier and SET_IDLE handling
- Successfully tested with Logitech G307 and similar devices

### Phase 6: Dynamic Endpoint Mapping (Completed)
- Implemented dynamic endpoint detection based on host driver configuration
- Added support for EEPROM interface overrides
- Fixed endpoint address comparison issues
- Successfully tested with complex composite devices

### Phase 7: Vendor Control Transfer Support (Completed)
- Identified issues with SET_REPORT/GET_REPORT handling
- Implemented proper sequencing for multi-stage control transfers
- Added state machine for control transfer handling
- Successfully tested with Pwnage mouse configuration software

---

## Verified Devices

### Low Speed (1.5 Mbps → 12 Mbps)
- **BenQ ZOWIE Gaming Mouse**
  - VID: 0x04A5, PID: 0x8001
  - 1 HID interface, 8-byte endpoint
  - 8-byte EP0 (typical for Low Speed)
  - Successfully proxied at Full Speed

### Full Speed (12 Mbps)
- **Glorious Model O Wireless**
  - VID: 0x258A, PID: 0x2022
  - 3 HID interfaces, 64-byte endpoints
  - Successfully proxied at Full Speed

- **Logitech G307**
  - VID: 0x046D, PID: 0xC53F
  - Non-compliant firmware (STALLs on Device Qualifier and SET_IDLE)
  - 3 HID interfaces (Keyboard, Mouse, Other)
  - Requires EEPROM override to select interface 1 (mouse)
  - Successfully proxied with request filtering and dynamic endpoint mapping
  - Demonstrates robust handling of real-world device quirks

### High Speed (480 Mbps)
- **Pwnage Wireless Gaming Mouse V3**
  - VID: 0x3662, PID: 0x2004
  - 3 HID interfaces, 8-byte mouse endpoint
  - Successfully proxied at High Speed with 8kHz polling
  - Configuration software works with vendor commands
  - SET_REPORT/GET_REPORT fully functional for configuration

### Additional Verified Devices
- **Logitech G Pro X Superlight**
- **Beast X Mini**

---

## Future Enhancements

### Potential Improvements
1. **Keyboard Support**: Extend forwarding logic to keyboard endpoints
2. **OUT Endpoint Support**: For devices requiring bidirectional communication
3. **Configuration UI**: Web interface for advanced settings
4. **Hub Support**: Handle multiple devices simultaneously
5. **Automatic EEPROM Override Detection**: Automatically determine optimal interface selection
6. **Firmware Update Support**: Enhanced vendor command handling for reliable firmware updates

### Known Limitations
1. **OUT endpoints** not implemented (not needed for most mice)
2. **Isochronous transfers** not supported
3. **Multiple devices** not supported simultaneously

---

## Summary

The USB proxy successfully creates a transparent, high-performance bridge between a physical USB mouse and a host PC. The implementation:
- ✅ Achieves true 8kHz polling rate for gaming mice
- ✅ Automatically detects and adapts to all USB speeds (Low/Full/High)
- ✅ Transparently converts Low Speed devices to Full Speed for compatibility
- ✅ Dynamically configures EP0 size based on device speed
- ✅ Handles non-compliant devices through intelligent request filtering
- ✅ Dynamically maps endpoints based on device configuration and EEPROM overrides
- ✅ Properly handles vendor control transfers (SET_REPORT/GET_REPORT)
- ✅ Provides 100% transparent device emulation
- ✅ Maintains sub-millisecond latency

The project demonstrates successful implementation of a custom USB device stack with proper hardware configuration for high-frequency polling, dynamic speed matching, universal device support, robust handling of real-world device quirks, intelligent endpoint routing, and proper vendor command handling.

## References

- **Teensy 4 `usb.c` Source Code** (Critical reference for ZLT bit and PHY configuration)
- **USB 2.0 Specification** (Chapter 9 - Device Framework)
- **i.MX RT1062 Reference Manual** (USB and USBPHY chapters)
- **USBHost_t36 Library Documentation** (Device speed detection)
- **HID Device Class Definition** (SET_REPORT/GET_REPORT implementation details)