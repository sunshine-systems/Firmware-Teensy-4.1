# USB Proxy Device Stack - Development & Implementation Guide

## Project Overview

### Goal
Create a **general-purpose HID input device proxy** for Teensy 4.1 that:
- Forwards HID mice and keyboards transparently to the host PC
- Supports complex composite devices (gaming mice/keyboards)
- Handles vendor-specific and proprietary protocols
- Supports media controls and system controllers
- Dynamically configures based on the connected device
- Supports all USB speeds and HID endpoint types
- Handles high-polling-rate gaming devices (up to 8kHz)
- Uses a polling-based architecture (no interrupts)

### Design Philosophy
**HID Device-Agnostic**: The proxy should work with any HID input device (mouse/keyboard) without modification. It discovers device capabilities and mirrors them exactly, including vendor-specific extensions.

## Update Log

### June 7, 2025 - String Descriptors Fixed! 🎉
- **FIXED**: String descriptors now appear correctly in Windows
- **Solution**: Send only actual descriptor length, not padded to 255/512 bytes
- **Result**: Proxy device now appears **100% identical** to real device
- **Remaining Issue**: Data forwarding implemented but not working

### Previous Updates
- **June 7, 2025 4:00 PM**: Endpoint configuration successful, enumeration completes
- **June 7, 2025 Morning**: Fixed symbol conflict, control transfers working
- **Earlier**: Completed Phase 0-2, USB PHY initialization, polling architecture

## Current Status Analysis

### What's Working ✅

1. **Complete Device Emulation**
   - Device appears 100% identical to real device in Windows
   - All string descriptors display correctly
   - VID/PID, configuration, interfaces all match
   - No errors in Windows Device Manager

2. **Symbol Conflict Resolution**
   - All proxy structures use unique names
   - No conflicts with usb.c
   - Hardware uses our initialized structures

3. **Control Transfer Proxy**
   - All descriptor requests forward perfectly
   - SET_ADDRESS handled locally
   - SET_CONFIGURATION works
   - HID descriptors retrieved

4. **Dynamic Endpoint Configuration**
   - Successfully parses configuration descriptor
   - Configures all 3 endpoints (0x81, 0x82, 0x83)
   - No more CLEAR_FEATURE failures

5. **Enumeration Complete**
   - Full USB enumeration sequence works
   - Windows recognizes as composite HID device
   - All 3 interfaces properly registered

### The Critical Issue 🔴

**Data Forwarding Not Working**

Despite implementing the data forwarding mechanism:
- Mouse data IS received from the physical device
- Forwarding code IS called
- But cursor doesn't move on PC

**Possible Causes**:
1. Endpoint not responding to IN tokens from host
2. Transfer descriptors not properly set up
3. Data not being queued at the right time
4. Missing some endpoint state management

## Architecture Overview

### Current Implementation

```
Physical Mouse ─USB─> Teensy 4.1 ─USB─> Host PC
                      │         │
                      │         └─> USB Device (proxy)
                      └─> USB Host (reads mouse)

Data Flow:
1. Physical mouse sends HID reports
2. USBHostDriver receives via interrupt transfer
3. Callback stores data in buffer
4. Main loop calls forwardMouseData()
5. USBDeviceProxy.sendDataOnEndpoint() queues transfer
6. PC should receive data on endpoint 0x81
```

### Key Components

1. **USBHostDriver** - Receives data from physical mouse ✅
2. **USBDeviceProxy** - Emulates device to PC ✅
3. **Data Forwarding** - Connects the two ❌

## Technical Deep Dive

### Why Data Forwarding Isn't Working

The logs show mouse data being received:
```
S: First data packet received! Length: 8 bytes: 00 00 01 00 00 00 00 00
I: Mouse - Buttons:0x00 X:1 Y:0 Wheel:0
```

But the forwarding mechanism may have issues:

1. **Endpoint State Management**
   ```cpp
   // Current implementation in sendDataOnEndpoint():
   USB1_ENDPTPRIME |= (1 << (ep_num + 16));
   endpoint_ready[ep_idx] = false;
   ```
   This might not be handling the USB protocol correctly.

2. **Transfer Descriptor Setup**
   The transfer descriptor might need different flags or setup.

3. **Timing Issues**
   The data might be queued when the host isn't expecting it.

## Implementation Status

### ✅ Phase 0-2: Foundation
- USB PHY initialization
- Polling architecture
- Basic control endpoint

### ✅ Phase 3: Control Path
- SETUP packet handling
- Descriptor forwarding
- Address assignment
- Configuration handling
- Dynamic endpoint configuration

### 🔧 Phase 4: Data Transfer Forwarding (90% Complete)
**Working:**
- Receiving data from physical device
- Parsing HID reports
- Callback mechanism
- Buffer management

**Not Working:**
- Actual forwarding to host PC
- Endpoint state machine for data endpoints
- Proper response to IN tokens

### 📋 Phase 5-6: Future Work
- Speed matching
- Advanced HID features
- High polling rate optimization

## Debugging Information

### Current Symptoms
1. Device enumerates perfectly
2. Appears identical to real device
3. Mouse data received from physical device
4. `forwardMouseData()` is called
5. But no cursor movement on PC

### Debug Points to Check
1. **Is endpoint responding to IN tokens?**
   - Add logging in `pollDataEndpoints()`
   - Check if `USB1_ENDPTCOMPLETE` is set for EP1

2. **Is transfer descriptor correct?**
   - Verify status bits
   - Check IOC (Interrupt on Complete) flag
   - Ensure proper buffer alignment

3. **Is data in the right format?**
   - Verify 8-byte HID report format
   - Check byte order

4. **Is timing correct?**
   - Data should be available when host polls
   - Not too early, not too late

## Code Architecture

```
USBDeviceProxy
├── Control Transfer Handler (✅ Complete)
├── Descriptor Parser (✅ Complete)
├── Endpoint Manager (✅ Complete)
│   ├── Parse descriptors ✅
│   ├── Configure endpoints ✅
│   └── Set up queue heads ✅
├── Data Forwarder (🔧 Needs Fix)
│   ├── IN endpoint handling ❌
│   ├── Transfer management ❌
│   └── State machine ❌
└── State Manager (✅ Complete)
    ├── Device states ✅
    ├── Connection tracking ✅
    └── Enumeration ✅
```

## Next Steps

### Immediate (Fix Data Forwarding)

1. **Add Detailed Logging**
   ```cpp
   void USBDeviceProxy::pollDataEndpoints() {
       // Log when host polls EP1
       // Log transfer completion
       // Log any errors
   }
   ```

2. **Verify Transfer Descriptor**
   ```cpp
   // In sendDataOnEndpoint()
   // Log all fields of transfer descriptor
   // Verify IOC bit is set
   // Check status after queueing
   ```

3. **Check Endpoint State Machine**
   - Ensure endpoint is ready when data arrives
   - Handle NAK properly
   - Queue data at the right time

4. **Test Simpler Approach**
   - Try continuous sending of static data
   - Verify endpoint works at all
   - Then add dynamic data

### Alternative Approaches

1. **Study Working USB Mouse Code**
   - Look at Teensy's usb_mouse.c
   - Understand how it handles IN endpoints
   - Apply same pattern to proxy

2. **Simplify First**
   - Get a single byte working
   - Then full HID report
   - Then continuous streaming

3. **Protocol Analyzer**
   - Use hardware USB analyzer if available
   - See actual USB transactions
   - Compare with working mouse

## Key Insights

### What We've Learned
1. String descriptor handling is critical for Windows
2. Endpoint configuration must match exactly
3. Control path and data path are very different
4. Timing is crucial for USB protocols

### Why We're Close
- Device enumeration is perfect
- All descriptors match
- Endpoints are configured
- Data is being received
- Just need to connect the final piece

## Summary for Next Session

**Current State**: The USB proxy successfully emulates a gaming mouse to the PC (appears identical in Device Manager) and receives data from the physical mouse. However, the data forwarding mechanism isn't working - mouse movements aren't reaching the PC.

**What Works**:
- Complete USB enumeration
- Perfect device emulation
- String descriptors fixed
- Endpoint configuration
- Receiving mouse data

**What Needs Fixing**:
- Data endpoint state machine
- Transfer descriptor handling
- Response to IN tokens from host

**Next Step**: Debug why configured endpoint 0x81 isn't successfully delivering mouse HID reports to the host PC when polled.

## References

- USB 2.0 Specification Chapter 9 (Device Framework)
- USB HID 1.11 Specification
- i.MX RT1062 Reference Manual (USB chapters)
- Teensy USB Stack (usb_mouse.c for endpoint handling)