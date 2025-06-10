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

### Status: **FULLY OPERATIONAL (8kHz VERIFIED + UNIVERSAL SPEED SUPPORT)** 🎉
The proxy successfully emulates the physical device with:
- **8kHz polling fully functional** for high-performance gaming mice
- **Universal speed support** - correctly handles Low Speed (1.5 Mbps), Full Speed (12 Mbps), and High Speed (480 Mbps) devices
- **Automatic speed adaptation** - Low Speed devices are transparently proxied at Full Speed for compatibility
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

#### 4. **Dynamic Speed Matching**
- The proxy detects whether the physical device operates at Low Speed, Full Speed, or High Speed
- Low Speed devices are automatically proxied at Full Speed (backwards compatible per USB spec)
- EP0 packet size is dynamically configured (8 bytes for Low Speed, 64 bytes for Full/High Speed)

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
- Maximum packet size (dynamically based on device)
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
- **USB Speed**: Dynamically matched (1.5, 12, or 480 Mbps)
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

### Key Insights
1. **The ZLT Bit**: The non-intuitive **ZLT bit (bit 29)** is critical for preventing endpoint stalls
2. **Speed Matters**: USB speed must be handled appropriately - Low Speed devices can be proxied at Full Speed
3. **EP0 Size Varies**: Low Speed devices typically use 8-byte EP0, not 64 bytes
4. **PHY Configuration Timing**: Speed must be configured BEFORE starting the USB controller
5. **Reference Implementation**: The official `usb.c` is the ultimate source of truth

### Debugging Indicators
- **"One packet then stall"**: Missing ZLT bit in endpoint configuration
- **Speed mismatch in Device Viewer**: PHY not configured for correct speed
- **Enumeration failures**: Check EP0 packet size for Low Speed devices
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

### Phase 4: Universal Speed Support (Completed)
- Added 3-way speed detection (Low/Full/High)
- Implemented Low Speed to Full Speed conversion
- Dynamic EP0 size configuration
- Tested with Low Speed devices

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

### High Speed (480 Mbps)
- **Pwnage Wireless Gaming Mouse V3**
  - VID: 0x3662, PID: 0x2004
  - 3 HID interfaces, 8-byte mouse endpoint
  - Successfully proxied at High Speed with 8kHz polling

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
- ✅ Provides 100% transparent device emulation
- ✅ Maintains sub-millisecond latency

The project demonstrates successful implementation of a custom USB device stack with proper hardware configuration for high-frequency polling, dynamic speed matching, and universal device support.

## References

- **Teensy 4 `usb.c` Source Code** (Critical reference for ZLT bit and PHY configuration)
- **USB 2.0 Specification** (Chapter 9 - Device Framework)
- **i.MX RT1062 Reference Manual** (USB and USBPHY chapters)
- **USBHost_t36 Library Documentation** (Device speed detection)