# USB Proxy Device Stack - Development & Implementation Guide

## Executive Summary

This guide outlines the development of a custom USB device stack for Teensy 4.1 that completely replaces the built-in USB functionality. The goal is to create a high-performance USB proxy that can match the polling rate of any connected device (up to 8kHz) while maintaining full compatibility with Windows, macOS, and Linux.

## Why This Architecture?

### Problem Statement
1. **ISR Context Deadlock**: The Teensy's USB device stack runs in interrupt context, preventing synchronous USB host operations
2. **Performance Limitations**: The built-in stack has a 4kHz output limitation even when reading 8kHz from devices
3. **Lack of Control**: Cannot modify descriptors or control packet flow at the required granularity
4. **Fixed Polling Rates**: Cannot dynamically match the connected device's polling rate

### Solution Rationale
- **Complete Stack Replacement**: Eliminates all ISR-based limitations
- **Polling-Based Design**: Allows synchronous USB host operations from main context
- **Direct Register Control**: Maximum performance with minimal overhead
- **Dynamic Configuration**: Adapts to any connected device's capabilities

## Development Phases

### Phase 0: Foundation & Research
**Goal**: Understand USB hardware and establish development environment

#### Tasks:
1. Study i.MX RT1062 USB module documentation (Chapter 41 of reference manual)
2. Map all USB registers and their functions
3. Create register access wrapper library
4. Set up debug output infrastructure (logic analyzer + Serial4)

#### Testing:
- Verify register read/write access
- Confirm USB PHY can be controlled manually
- Test timing measurement accuracy

#### Key Decisions:
- Use volatile pointers for register access (fastest method)
- Implement register logging for debugging
- Create USB analyzer setup for packet verification

---

### Phase 1: Disable Teensy USB Stack
**Goal**: Completely disable built-in USB without affecting USB host

#### Understanding Current Stack:
```
Teensy USB Stack Components:
- usb.c: Core USB device implementation
- usb_dev.c/h: Device-specific code
- USBHost_t36: USB host library (keep this!)
```

#### Implementation Steps:
1. **Prevent USB initialization**
   - Remove `usb_init()` call from startup
   - Ensure `USB1_USBCMD` stays cleared
   - Disable USB interrupts (`NVIC_DISABLE_IRQ(IRQ_USB1)`)

2. **Verify USB host still works**
   - USB host uses USB2 module (different from device USB1)
   - Confirm mouse/keyboard enumeration still functions

3. **Clean shutdown procedure**
   ```
   Steps:
   1. Disable USB interrupts
   2. Stop USB controller (USB1_USBCMD = 0)
   3. Disable USB clocks if needed
   4. Reset USB PHY to clean state
   ```

#### Testing:
- Confirm no USB device appears on host computer
- Verify USB host can still enumerate devices
- Check power consumption is reduced
- Monitor for any spurious USB activity

---

### Phase 2: USB PHY Initialization
**Goal**: Manually initialize USB hardware for device mode

#### Critical Understanding:
- USB PHY must be configured before controller
- Timing is critical - follow exact sequence from datasheet
- Different initialization for HS vs FS modes

#### Initialization Sequence:
1. **Clock Configuration**
   - Enable USB clocks via CCM
   - Configure PLL for USB timing
   - Wait for PLL lock

2. **PHY Reset and Configuration**
   - Assert PHY reset
   - Configure PHY parameters
   - Release reset
   - Wait for PHY ready

3. **Controller Configuration**
   - Set device mode
   - Configure endpoint buffers
   - Enable pull-up resistor

#### Testing:
- Measure USB D+/D- voltages
- Verify host computer detects device connection
- Check for proper speed negotiation
- Capture USB reset sequence

---

### Phase 3: Control Endpoint Implementation
**Goal**: Handle basic USB enumeration

#### Design Decisions:
- **Polling-based**: Check for SETUP packets in main loop
- **State machine**: Track enumeration state
- **Minimal buffering**: Direct pass-through where possible

#### Implementation Stages:

##### 3.1: SETUP Packet Detection
```
Loop continuously:
1. Check ENDPTSETUPSTAT register
2. Read SETUP packet data
3. Clear SETUP status bit
4. Process or forward request
```

##### 3.2: Descriptor Handling
```
Descriptor Flow:
1. Receive GET_DESCRIPTOR from host
2. Forward to connected device via USB host
3. Modify descriptor if needed (polling rate, etc.)
4. Send response to host computer
```

##### 3.3: Standard Request Processing
- **SET_ADDRESS**: Must handle locally (timing critical)
- **SET_CONFIGURATION**: Forward and mirror state
- **GET_STATUS**: May need local handling
- **CLEAR_FEATURE**: Forward to device

#### Testing Checkpoints:
1. **SETUP Reception**: Log all SETUP packets
2. **Device Descriptor**: Verify correct VID/PID
3. **Configuration Descriptor**: Check endpoint configuration
4. **Full Enumeration**: Device appears in OS
5. **Timing Verification**: Meet USB timing requirements

---

### Phase 4: Dynamic Endpoint Configuration
**Goal**: Configure endpoints based on connected device

#### Key Concepts:
- Endpoint configuration must match connected device
- Support variable packet sizes (8, 16, 32, 64 bytes)
- Handle different endpoint types (Control, Interrupt, Bulk, Iso)

#### Implementation:
1. **Parse Configuration Descriptor**
   - Extract endpoint information
   - Determine polling intervals
   - Identify HID interfaces

2. **Configure Endpoint Hardware**
   - Set endpoint type in ENDPTCTRL registers
   - Configure max packet size
   - Set up double buffering

3. **Polling Rate Adaptation**
   ```
   Device reports 1ms (1000Hz) → We configure 1ms
   Device reports 0.125ms (8000Hz) → We configure 0.125ms
   Device reports 10ms (100Hz) → We configure 10ms
   ```

#### Testing:
- Test with 1kHz, 4kHz, 8kHz mice
- Verify keyboard 1kHz operation
- Test with composite devices
- Measure actual polling rates

---

### Phase 5: High-Performance Data Transfer
**Goal**: Achieve native polling rates with minimal latency

#### Design Principles:
1. **Zero-copy architecture**: DMA directly between endpoints
2. **Minimal processing**: Fast path for unmodified data
3. **Interrupt coalescing**: Reduce overhead
4. **Predictable timing**: Consistent latency

#### Polling Loop Structure:
```
Main Loop (target: 16kHz minimum):
1. Check control endpoint (highest priority)
2. Process pending host requests
3. Check IN endpoints (device → host)
4. Check OUT endpoints (host → device)
5. Handle error conditions
```

#### Optimization Techniques:
- **Register caching**: Read volatile registers once per loop
- **Branch prediction**: Order checks by likelihood
- **Inline functions**: Eliminate call overhead
- **DMA chaining**: Prepare next transfer in advance

#### Testing:
- Measure loop frequency with oscilloscope
- Test data integrity at maximum rates
- Verify no packet loss under load
- Profile CPU usage

---

### Phase 6: HID-Specific Features
**Goal**: Full HID protocol support

#### Required Features:
1. **HID Descriptor Parsing**
   - Extract report format
   - Identify report IDs
   - Parse feature reports

2. **Report Processing**
   - GET_REPORT requests
   - SET_REPORT requests
   - Feature reports
   - Output reports (LEDs)

3. **Boot Protocol Support**
   - Simplified 8-byte format
   - BIOS compatibility
   - Protocol switching

#### Testing:
- Test with various mice/keyboards
- Verify BIOS/UEFI compatibility
- Test report rate changes
- Verify LED control (keyboards)

---

### Phase 7: Error Handling & Recovery
**Goal**: Robust operation under all conditions

#### Error Scenarios:
1. **Device Disconnection**
   - Detect removal quickly
   - Clean up state
   - Notify host appropriately

2. **Timing Violations**
   - Track timing requirements
   - Implement proper NAK responses
   - Recover from errors

3. **Buffer Management**
   - Prevent overflows
   - Handle underruns
   - Maintain data integrity

#### Implementation:
- Watchdog timers for stuck states
- Error counters and thresholds
- Automatic recovery procedures
- Diagnostic reporting

---

### Phase 8: Performance Validation
**Goal**: Verify system meets all requirements

#### Test Suite:
1. **Latency Testing**
   - End-to-end latency measurement
   - Jitter analysis
   - Worst-case scenarios

2. **Throughput Testing**
   - Maximum polling rate verification
   - Sustained transfer rates
   - CPU usage profiling

3. **Compatibility Testing**
   - Windows 10/11
   - macOS
   - Linux
   - Various BIOS/UEFI

4. **Stress Testing**
   - Long duration operation
   - Rapid connect/disconnect
   - Multiple devices
   - Error injection

---

## Architecture Decisions & Rationale

### 1. No ISR Usage
**Decision**: Poll everything from main loop
**Rationale**: 
- Eliminates context switching overhead
- Allows synchronous USB host operations
- Predictable timing behavior
- Simpler debugging

### 2. Direct Register Manipulation
**Decision**: Bypass all Arduino/Teensy USB abstractions
**Rationale**:
- Maximum performance
- Complete control over timing
- Ability to implement non-standard behavior
- Minimal code overhead

### 3. Dynamic Configuration
**Decision**: Adapt to connected device capabilities
**Rationale**:
- Support any HID device
- Optimal performance for each device
- Future-proof design
- Market compatibility

### 4. State Machine Design
**Decision**: Explicit state tracking for all operations
**Rationale**:
- Predictable behavior
- Easy error recovery
- Clear debugging path
- Formal verification possible

---

## Risk Mitigation

### Technical Risks:
1. **Timing Requirements**
   - Mitigation: Extensive timing analysis and testing
   - Fallback: Implement NAK-based flow control

2. **Hardware Quirks**
   - Mitigation: Study errata sheets thoroughly
   - Fallback: Work with NXP support if needed

3. **OS Compatibility**
   - Mitigation: Test on multiple OS versions
   - Fallback: Implement quirks mode for specific OS

### Project Risks:
1. **Complexity**
   - Mitigation: Incremental development with testing
   - Fallback: Can revert to specific phases

2. **Performance Goals**
   - Mitigation: Profile early and often
   - Fallback: Identify acceptable performance tiers

---

## Success Criteria

### Minimum Viable Product:
- [ ] Enumerate as HID device
- [ ] Forward mouse data at 1kHz
- [ ] Forward keyboard data
- [ ] Stable operation for 1 hour

### Target Product:
- [ ] Support 8kHz mice
- [ ] < 0.5ms added latency
- [ ] Zero packet loss
- [ ] Hot-plug support
- [ ] Multi-OS compatibility

### Stretch Goals:
- [ ] Support multiple devices
- [ ] Composite device support
- [ ] Custom report modification
- [ ] Configuration UI

---

## Development Tools & Resources

### Required Tools:
1. **Logic Analyzer**: USB protocol decode capable
2. **Oscilloscope**: For timing measurements
3. **USB Analyzer**: Software or hardware
4. **Test Devices**: Various mice/keyboards at different polling rates

### Reference Documents:
1. i.MX RT1062 Reference Manual (Chapter 41)
2. USB 2.0 Specification
3. HID 1.11 Specification
4. Teensy 4.1 Schematic
5. NXP USB Stack Examples

### Debug Infrastructure:
1. Register dump functions
2. Packet logging system
3. Timing measurement points
4. State machine visualization

---

## Testing Philosophy

### Principles:
1. **Test at every phase** - Don't accumulate technical debt
2. **Automate where possible** - Repeatable results
3. **Test edge cases** - Not just happy path
4. **Performance from day one** - Don't assume optimization later

### Test Categories:
1. **Unit Tests**: Individual functions
2. **Integration Tests**: Component interaction
3. **System Tests**: Full stack operation
4. **Performance Tests**: Timing and throughput
5. **Stress Tests**: Extended operation

---

## Common Pitfalls to Avoid

1. **Assuming USB is simple** - It's a complex protocol with many edge cases
2. **Ignoring timing requirements** - USB has strict timing windows
3. **Buffer management errors** - Common source of crashes
4. **State synchronization issues** - Device and host must agree
5. **Polling too slowly** - Must exceed device's rate
6. **Not handling errors** - USB errors will happen
7. **Forgetting about power management** - Devices may sleep
8. **OS-specific quirks** - Each OS has unique behavior

---

## Conclusion

This architecture provides complete control over USB device behavior while maintaining compatibility with existing USB host code. By replacing the ISR-based stack with a polling design, we eliminate the fundamental limitation that prevented high-performance proxying.

The phased approach allows for incremental development with testing at each stage, reducing risk and ensuring a solid foundation for the high-performance requirements of 8kHz mouse support.