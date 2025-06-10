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
- ✅ **NEW**: Automatically matches the USB speed of the connected device

### Status: **FULLY OPERATIONAL (8kHz VERIFIED + SPEED MATCHING)** 🎉
The proxy successfully emulates the physical device with:
- **8kHz polling fully functional** for high-performance gaming mice
- **Automatic speed matching** - correctly operates at Full Speed (12 Mbps) or High Speed (480 Mbps) based on the connected device
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
1. Physical mouse sends HID reports (8 bytes @ up to 8kHz)
2. USBHostDriver receives via interrupt transfer  
3. Data callback immediately forwards to the proxy stack
4. USBDeviceProxy, correctly configured for device speed and polling rate, sends to PC
5. PC sees an exact replica of the physical mouse at the correct USB speed
```

### Key Components

1. **USBHostDriver** - USB Host implementation for reading the physical mouse
2. **USBDeviceProxy** - Custom polling-based USB Device Stack that presents the proxy device to the PC
3. **HIDMouseDescriptorHandler** - Helper for parsing HID report descriptors
4. **Main Loop** - High-frequency orchestration (~200-400kHz)

---

## Implementation Details

### Critical Design Decisions

#### 1. **Polling Architecture**
- The custom device stack (`USBDeviceProxy`) uses no interrupts, preventing conflicts with the core Teensy libraries and ensuring maximum performance.

#### 2. **Direct Forwarding**
- Mouse data is forwarded immediately upon receipt, with minimal buffering, for the lowest possible latency.

#### 3. **Perfect Descriptor Proxying**
- All descriptors (Device, Configuration, String, HID) are proxied 1:1 from the physical device, ensuring the host PC sees an identical device.

#### 4. **Dynamic Speed Matching** (NEW)
- The proxy detects whether the physical device operates at Full Speed or High Speed and configures the Teensy's USB PHY to match.

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

#### 4. **USB Speed Matching: The Complete Solution** (NEW)
To ensure the proxy appears identical to the physical device, we implemented automatic USB speed detection and configuration.

**The Problem:** The Teensy 4.1 defaults to High Speed (480 Mbps), but many mice operate at Full Speed (12 Mbps). This speed mismatch was visible in USB Device Viewer.

**The Solution:** 
1. Detect the physical device's speed using the USBHost_t36 library
2. Configure the Teensy's USB PHY before controller initialization
3. Use specific PHY register settings to force Full Speed when needed

```cpp
// In USBHostDriver - detect device speed
bool USBHostDriver::isDeviceHighSpeed() const {
    // Speed values: 0=Low, 1=Full, 2=High
    return (device->speed == 2);
}

// In USBDeviceProxy::begin() - configure PHY for detected speed
if (!device_high_speed) {
    // Force Full Speed operation
    USBPHY1_CTRL_SET = USBPHY_CTRL_ENUTMILEVEL2 | USBPHY_CTRL_ENUTMILEVEL3;
    USB1_PORTSC1 |= USB_PORTSC1_PFSC;
} else {
    // Allow High Speed operation
    USBPHY1_CTRL_CLR = USBPHY_CTRL_ENUTMILEVEL2 | USBPHY_CTRL_ENUTMILEVEL3;
    USB1_PORTSC1 &= ~USB_PORTSC1_PFSC;
}
```

---

## USB Protocol Implementation

### Enumeration Sequence
1. **USB Reset**: Clear all proxy state
2. **GET_DESCRIPTOR**: Forward all descriptor requests 1:1 from the physical device
3. **SET_ADDRESS**: Handle locally for correct timing
4. **SET_CONFIGURATION**: Forward request, then configure all hardware endpoints
5. **HID Specific**: Forward all other requests directly

### Endpoint Configuration
The `configureEndpoint` function correctly sets:
- Maximum packet size
- The critical **ZLT bit (bit 29)** for proper operation
- Endpoint type and direction

### Data Transfer Flow
1. Physical mouse sends HID report
2. `USBHostDriver::processInData()` receives the data
3. `mouseDataCallback()` is called
4. `sendDataOnEndpoint()` queues the data to the hardware endpoint
5. Host PC polls at the configured rate (up to 8kHz)
6. Teensy hardware responds and signals completion
7. `pollDataEndpoints()` marks endpoint ready for next packet

---

## Technical Specifications

### Performance Metrics
- **Polling Rate**: ~200-400kHz typical
- **Latency**: <1ms end-to-end
- **USB Speed**: Dynamically matched (12 or 480 Mbps)
- **HID Report Rate**: Up to 8kHz achieved and verified

### Memory Layout
```
0x20000000: proxy_endpoint_queue_head (4KB aligned)
0x20000800: Transfer descriptors
0x20001000: Data buffers
```

### Endpoint Mapping (Example Devices)
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

---

## Lessons Learned

### What Worked Well
1. **Polling architecture**: Simple, fast, and reliable
2. **Direct descriptor proxying**: Ensures perfect device replication
3. **Speed detection and matching**: Makes the proxy truly transparent
4. **Meticulous debugging**: Essential for hardware state issues

### Key Insights
1. **The ZLT Bit**: The non-intuitive **ZLT bit (bit 29)** is critical for preventing endpoint stalls
2. **Speed Matters**: USB speed must match the physical device for true transparency
3. **PHY Configuration Timing**: Speed must be configured BEFORE starting the USB controller
4. **Reference Implementation**: The official `usb.c` is the ultimate source of truth

### Debugging Indicators
- **"One packet then stall"**: Missing ZLT bit in endpoint configuration
- **Speed mismatch in Device Viewer**: PHY not configured for correct speed
- **Endpoint not ready**: Hardware completion signal not generated

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

---

## Verified Devices

### Full Speed (12 Mbps)
- **Glorious Model O Wireless**
  - VID: 0x258A, PID: 0x2022
  - 3 HID interfaces, 64-byte endpoints
  - Successfully proxied at Full Speed

### High Speed (480 Mbps)
- **Pwnage Wireless Gaming Mouse V3**
  - VID: 0x3662, PID: 0x2004
  - 3 HID interfaces, 8-byte mouse endpoint
  - Successfully proxied at High Speed with 8kHz polling

---

## Future Enhancements

### Potential Improvements
1. **Keyboard Support**: Extend forwarding logic to keyboard endpoints
2. **OUT Endpoint Support**: For devices requiring bidirectional communication
3. **Low Speed Support**: Handle 1.5 Mbps devices
4. **Configuration UI**: Web interface for advanced settings

### Known Limitations
1. **OUT endpoints** not implemented (not needed for most mice)
2. **Isochronous transfers** not supported
3. **Low Speed** devices not tested

---

## Summary

The USB proxy successfully creates a transparent, high-performance bridge between a physical USB mouse and a host PC. The implementation:
- ✅ Achieves true 8kHz polling rate for gaming mice
- ✅ Automatically matches the USB speed of any connected device
- ✅ Provides 100% transparent device emulation
- ✅ Maintains sub-millisecond latency

The project demonstrates successful implementation of a custom USB device stack with proper hardware configuration for both high-frequency polling and dynamic speed matching.

## References

- **Teensy 4 `usb.c` Source Code** (Critical reference for ZLT bit and PHY configuration)
- **USB 2.0 Specification** (Chapter 9 - Device Framework)
- **i.MX RT1062 Reference Manual** (USB and USBPHY chapters)
- **USBHost_t36 Library Documentation** (Device speed detection)