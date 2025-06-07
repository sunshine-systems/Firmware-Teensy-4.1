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

### June 7, 2025 - Major Breakthrough & Analysis
- **Fixed**: Symbol conflict resolved - proxy now uses properly initialized queue heads
- **Working**: Control transfers, descriptor forwarding, enumeration progress
- **Issue Identified**: Enumeration fails at CLEAR_FEATURE(ENDPOINT_HALT) on endpoint 0x81
- **Root Cause**: Proxy hasn't configured data endpoints yet
- **Next Step**: Implement dynamic endpoint configuration based on device descriptors

### Previous Updates
- **June 7, 2025 5:12 AM**: Attempted section attribute fix (didn't work)
- **June 7, 2025 4:51 AM**: Identified ENDPTPRIME timeout issue
- **Earlier**: Completed Phase 0-2, USB PHY initialization, polling architecture

## Current Status Analysis

### What's Working ✅
1. **Symbol Conflict Resolution**
   - All proxy structures use unique names (`proxy_endpoint_queue_head`, etc.)
   - Hardware correctly uses our initialized structures
   - No more ENDPTPRIME timeouts

2. **Control Transfer Proxy**
   - Successfully forwards all descriptor requests
   - Handles SET_ADDRESS locally (timing critical)
   - Forwards SET_CONFIGURATION
   - Retrieves and forwards HID descriptors

3. **Enumeration Progress**
   ```
   GET_DESCRIPTOR(Device) → SET_ADDRESS → GET_DESCRIPTOR(Device) → 
   GET_DESCRIPTOR(Config) → GET_DESCRIPTOR(Strings) → SET_CONFIGURATION →
   GET_DESCRIPTOR(HID) → CLEAR_FEATURE(EP1) ❌
   ```

### The Critical Issue 🔴

**Enumeration fails at**: `CLEAR_FEATURE(ENDPOINT_HALT)` on endpoint 0x81

**Why this happens**:
1. PC thinks endpoint 1 is configured (because we said configuration succeeded)
2. Proxy hasn't actually configured any data endpoints in hardware
3. Request forwarded to mouse causes state confusion
4. Mouse stops responding to control transfers

## Architecture Overview

### The General-Purpose USB Proxy

```
Any USB Device ─USB─> Teensy 4.1 ─USB─> Host PC
                      │         │
                      │         └─> USB Device (our proxy)
                      └─> USB Host (existing driver)

Key Components:
1. Device Detection: Identify speed, class, endpoints
2. Dynamic Configuration: Mirror exact device structure
3. Transparent Forwarding: All transfers pass through unchanged
4. State Management: Keep both sides synchronized
```

### Current Implementation Gaps

1. **Static Endpoint 0 Only**: Only control endpoint implemented
2. **No Dynamic Configuration**: Doesn't parse descriptors to configure endpoints
3. **No Speed Matching**: Assumes high-speed
4. **No Bulk/Interrupt/Iso Support**: Only control transfers work

## Technical Deep Dive

### The Missing Piece: Dynamic Endpoint Configuration

When the PC sends SET_CONFIGURATION, the proxy needs to:

1. **Parse the configuration descriptor** to find all endpoints:
   ```cpp
   // From the logs, this mouse has:
   Interface 0: HID Mouse
   - Endpoint 0x81 (IN, Interrupt, 8 bytes, 1ms interval)
   
   Interface 1: HID Keyboard  
   - Endpoint 0x82 (IN, Interrupt, 64 bytes, 1ms interval)
   
   Interface 2: HID Other
   - Endpoint 0x83 (IN, Interrupt, 64 bytes, 1ms interval)
   ```

2. **Configure matching endpoints** in Teensy hardware:
   ```cpp
   // For each endpoint found:
   uint32_t ep_num = endpoint_address & 0x0F;
   uint32_t ep_dir = (endpoint_address & 0x80) ? 1 : 0;
   
   // Configure endpoint queue head
   proxy_endpoint_queue_head[ep_num * 2 + ep_dir].config = 
       (max_packet_size << 16) | endpoint_flags;
   
   // Configure endpoint control register
   uint32_t ctrl = USB1_ENDPTCTRL0 + ep_num;
   // Set type, direction, enable
   ```

3. **Start data forwarding** for each endpoint

### Why Device-Agnostic Matters

Different HID input devices need different handling:
- **Basic HID Mouse**: 1 endpoint, interrupt transfers, 8ms intervals
- **Gaming Mouse**: 3-4 endpoints, 1000Hz+ polling, vendor extensions
- **Keyboard with Media Keys**: Multiple interfaces, different report types
- **RGB Gaming Devices**: HID + vendor-specific endpoints
- **Composite Devices**: Mouse + keyboard + media + RGB in one device

The proxy must dynamically adapt to each device configuration.

## Implementation Phases (Revised)

### ✅ Phase 0-2: Foundation Complete
- USB PHY initialization
- Polling architecture (1.2-5.9 MHz rate)
- Basic control endpoint

### ✅ Phase 3a: Control Endpoint (90% Complete)
- SETUP packet handling ✅
- Descriptor forwarding ✅
- SET_ADDRESS handling ✅
- Missing: Endpoint-specific request handling

### 🔧 Phase 3b: Dynamic Endpoint Configuration (Current)
**This is the critical missing piece!**

1. **Parse Configuration Descriptor**
   ```cpp
   void parseConfigurationDescriptor(uint8_t* desc, uint16_t len) {
       // Walk through all descriptors
       // Find each endpoint descriptor
       // Store endpoint info (address, type, size, interval)
   }
   ```

2. **Configure Hardware Endpoints**
   ```cpp
   void configureEndpoint(uint8_t addr, uint8_t type, uint16_t size) {
       uint8_t num = addr & 0x0F;
       uint8_t dir = (addr & 0x80) ? 1 : 0;
       
       // Set up queue head
       // Configure ENDPTCTRL register
       // Enable endpoint
   }
   ```

3. **Handle Endpoint Requests Locally**
   ```cpp
   // Don't forward these to the device:
   case CLEAR_FEATURE:
       if (wIndex & 0x0F) { // Endpoint number > 0
           // Handle locally
           clearEndpointHalt(wIndex);
           sendZLP(); // ACK
           return;
       }
   ```

### 📋 Phase 4: Data Transfer Forwarding
- Set up transfer descriptors for each endpoint
- Poll for IN data from device
- Forward to PC
- Handle OUT data from PC to device

### 📋 Phase 5: Speed Matching
- Detect device speed from USB host
- Configure USB device controller to match
- Handle speed-specific timing

### 📋 Phase 6: Advanced HID Features
- Vendor-specific protocol support
- High polling rate optimization (8kHz+)
- Composite device synchronization
- RGB and proprietary extensions

## Next Steps (Immediate)

### 1. Implement Descriptor Parser
```cpp
struct EndpointInfo {
    uint8_t address;
    uint8_t attributes; // type, sync, usage
    uint16_t maxPacketSize;
    uint8_t interval;
};

void parseEndpoints(uint8_t* configDesc, uint16_t len, 
                   EndpointInfo* endpoints, uint8_t* count);
```

### 2. Add Endpoint Configuration
```cpp
void USBDeviceProxy::configureEndpoints(EndpointInfo* endpoints, uint8_t count) {
    for (int i = 0; i < count; i++) {
        configureEndpoint(endpoints[i]);
    }
}
```

### 3. Fix CLEAR_FEATURE Handling
```cpp
// In handleSetupPacket:
if (pending_setup.bRequest == 0x01 && // CLEAR_FEATURE
    pending_setup.bmRequestType == 0x02) { // Endpoint recipient
    // Handle locally, don't forward
    handleClearFeature();
    return;
}
```

### 4. Implement Data Forwarding
- Create transfer descriptors for each endpoint
- Set up polling mechanism
- Forward data bidirectionally

## Performance Considerations

### Current Performance
- Polling rate: 1.2-5.9 MHz (excellent)
- Control transfer latency: ~50ms (includes forwarding)
- Overhead: Minimal due to polling design

### For General-Purpose Support
- Must handle varying endpoint intervals (125μs to 255ms)
- Support different packet sizes (8 to 1024 bytes)
- Manage multiple concurrent transfers

## Testing Strategy

### Device Compatibility Matrix
| Device Type | Endpoints | Speed | Status |
|------------|-----------|-------|--------|
| Standard HID Mouse | 1 INT | FS/HS | In Progress |
| Gaming Mouse (Multi-Interface) | 3-4 INT | FS/HS | In Progress |
| HID Keyboard | 1-2 INT | FS/HS | Pending |
| Gaming Keyboard (Composite) | 2-5 INT | FS/HS | Pending |
| Media Controller Keys | 1-2 INT | FS | Pending |
| System Control (Power/Sleep) | 1 INT | FS | Pending |
| Vendor-Specific Mouse/KB | Variable | FS/HS | Pending |
| RGB Controller Interface | 1-2 INT/BULK | FS/HS | Pending |

### Validation Steps
1. **Fix current mouse** - Complete endpoint configuration for 3-interface gaming mouse
2. **Test simple HID mice** - Basic 1-endpoint mice
3. **Test gaming keyboards** - Multi-interface with media keys
4. **Test vendor-specific** - Mice/keyboards with proprietary protocols
5. **Test composite devices** - Devices with RGB, macro, and standard HID interfaces

## Key Insights

### Why It Almost Works
- Control path is perfect
- Descriptors forward correctly
- Timing requirements met
- Only missing: data endpoint configuration

### The Path Forward
1. **Short term**: Get current gaming mouse working (add endpoint config)
2. **Medium term**: Support all HID input devices and composites
3. **Long term**: Perfect compatibility with gaming peripherals and proprietary protocols

### Critical Success Factors
1. **Dynamic configuration** - Adapt to any device
2. **State synchronization** - Keep both sides consistent
3. **Timing accuracy** - Meet USB specifications
4. **Resource management** - Handle Teensy's endpoint limits

## Code Architecture (Proposed)

```
USBDeviceProxy
├── Control Transfer Handler (✅ Done)
├── Descriptor Parser (🔧 Needed)
├── Endpoint Manager (🔧 Needed)
│   ├── Configure from descriptors
│   ├── Allocate queue heads
│   └── Set up transfers
├── Data Forwarder (📋 TODO)
│   ├── IN polling
│   ├── OUT handling
│   └── Transfer management
└── State Manager (📋 TODO)
    ├── Device state
    ├── Endpoint states
    └── Error recovery
```

## References

- USB 2.0 Specification Chapter 9 (Device Framework)
- USB HID 1.11 Specification
- i.MX RT1062 Reference Manual (USB chapters)
- USB in a NutShell (endpoint configuration)
- Gaming Mouse Protocol Analysis (community resources)

## Context for Next Development Session

We've proven the proxy concept works - control transfers forward perfectly and the PC nearly completes enumeration. The critical missing piece is dynamic endpoint configuration. Once we parse the configuration descriptor and set up the matching endpoints in hardware, the proxy should work with any USB device. The architecture is sound, the performance is excellent, and we're one implementation step away from a working general-purpose USB proxy.