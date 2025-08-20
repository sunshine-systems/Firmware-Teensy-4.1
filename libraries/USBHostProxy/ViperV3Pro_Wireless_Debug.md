# Razer Viper V3 Pro Wireless Dongle Debug Documentation

## Issue Summary
The Razer Viper V3 Pro wireless dongle (VID:0x1532 PID:0x00C1) fails control transfers with the USB Host Proxy, while the same mouse works fine in wired mode (VID:0x1532 PID:0x00C0).

## Error Pattern Analysis

### Primary Error Signature
- **Token Value**: `0x00008001`
- **Error Location**: Control transfers during enumeration and Windows descriptor requests
- **Speed**: High Speed (480 Mbps) for wireless, Full Speed (12 Mbps) for wired

### Token Bit Breakdown
When token = `0x00008001`:
- **Status byte (bits 0-7)**: `0x01`
  - Bit 0 (Ping State): 1 - Set, indicating an error
  - Bit 1 (Split State): 0
  - Bit 2 (Missed Microframe): 0
  - Bit 3 (Transaction Error): 0
  - Bit 4 (Babble): 0
  - Bit 5 (Data Buffer Error): 0
  - Bit 6 (Halted): 0
  - Bit 7 (Active): 0 - Not active (transaction complete)
- **PID Code (bits 8-9)**: 0
- **CERR (bits 10-11)**: 0 - Error counter
- **Total Bytes (bits 16-30)**: Varies
- **Data Toggle (bit 31)**: Varies

## Failure Points

### Phase 1: Initial Enumeration (Teensy → Device)
```
Line 180: Control transfer: bmRequestType=0x81 bRequest=0x06 wValue=0x2200 wIndex=0x0000 wLength=94
Line 184: Control transfer completed with token=0x00008001, length=94
Line 185: SunBox USB Host Driver Error: Control transfer error, token=0x00008001
```
- Fails when requesting HID descriptor for interface 0
- Falls back to boot protocol mode

### Phase 2: Windows Enumeration (Windows → Teensy → Device)
```
Line 224: Control transfer: bmRequestType=0x80 bRequest=0x06 wValue=0x0100 wIndex=0x0000 wLength=64
Line 228: Control transfer completed with token=0x00008001, length=64
Line 229: SunBox USB Host Driver Error: Control transfer error, token=0x00008001
```
- Fails when Windows requests device descriptor through proxy
- Windows retries multiple times before giving up

## Working Scenarios Comparison

### Pwnage Max (Wireless - Working)
- High Speed (480 Mbps)
- Token values: `0x00008000` or `0x0000800` (success)
- All control transfers succeed

### Viper V3 Pro (Wired - Working)
- Full Speed (12 Mbps)
- Token values: `0x00008000` (success)
- All control transfers succeed

## Debug Logging Enhancements Added

### 1. USBHostDriver.cpp
- Added timestamp logging for transfer duration measurement
- Added raw setup packet byte logging
- Enhanced token bit decoding with detailed status bits
- Added control buffer content logging for failed transfers
- Added device address and pipe logging

### 2. USBDeviceProxy.cpp
- Added control stage transition logging
- Added transfer timing measurements
- Added request type classification (Standard/Class, Device/Interface)
- Enhanced STALL logging with failed request details
- Added forwarding status logging

### 3. ehci.cpp (USBHost_t36)
- Enabled setup packet detail logging
- Enabled queue_Transfer result logging
- Enabled raw setup packet data hex dump

## Next Steps for Investigation

### With the enhanced logging, monitor:

1. **Timing Patterns**
   - Compare transfer durations between working and failing devices
   - Check if wireless dongle has stricter timing requirements

2. **Setup Packet Differences**
   - Verify setup packet bytes are correct
   - Check for any corruption in the control setup structure

3. **USB Controller State**
   - Monitor EHCI controller status during failures
   - Check for Split Transaction issues with High Speed

4. **Error Recovery**
   - Note if CERR counter increments (indicating retries)
   - Check if specific descriptor types always fail

### Potential Root Causes to Investigate:

1. **High Speed Hub Issues**
   - Wireless dongle may use internal hub with Split Transactions
   - EHCI Split Transaction handling may have timing issues

2. **NAK Handling**
   - Device may NAK more frequently in wireless mode
   - Host controller may not handle NAKs properly at High Speed

3. **Microframe Timing**
   - High Speed uses 125μs microframes vs 1ms frames
   - Timing violations may cause Ping State errors

4. **Power Management**
   - Wireless dongle may enter low power states
   - May need different wake-up timing

## Test Procedures

1. **Capture Full Debug Log**
   - Connect Viper V3 Pro wireless dongle
   - Enable debug mode if not already enabled
   - Capture complete enumeration sequence
   - Note all token values and timing

2. **Compare with Working Device**
   - Repeat with Pwnage Max or wired Viper
   - Compare setup packet sequences
   - Note timing differences

3. **Analyze Failure Pattern**
   - Check if failures are consistent or intermittent
   - Look for specific descriptor types that fail
   - Monitor error recovery attempts

## Log File References
- Working: `Pwnage_Max_cf.txt` (High Speed wireless)
- Working: `ViperV3Pro_Wired_Teensy.txt` (Full Speed wired)
- Failing: `ViperV3Pro_Wireless_Teensy.txt` (High Speed wireless)

## Critical Discovery (2024-01-20)

### THE DATA IS ACTUALLY BEING RECEIVED!

Analysis of debug logs revealed a crucial finding:
- Control transfers marked as failed (token=0x00008001) **actually contain valid data**
- Example: HID descriptor request shows `Control buffer: 05 01 09 02` - valid HID descriptor start
- Example: Device descriptor shows `Control buffer: 12 01 00 02` - valid device descriptor start

### The Real Problem: Ping Protocol False Positive

1. **What's Actually Happening**:
   - USB controller successfully receives data from device
   - Data is correctly placed in control buffer
   - Transfer is incorrectly marked failed due to Ping State bit
   - Our driver sees the error and rejects valid data

2. **Why Only This Device**:
   - Razer Viper V3 Pro wireless has unique ping protocol timing
   - Works during enumeration (USBHost_t36 internal code)
   - Fails with our custom control transfers
   - Only affects High Speed (480 Mbps) mode

3. **Evidence**:
   ```
   Token: 0x00008001
   Status bits: Active=0, Halted=0, DataBufErr=0, Babble=0, 
                XactErr=0, MissedMicro=0, Split=0, Ping=1
   Control buffer: 05 01 09 02 (VALID HID DATA!)
   ```

## Solution Strategy

### Solution 1: Accept Data Despite Ping Error ✅ IMPLEMENTED & VERIFIED
**Rationale**: If we have valid data, the transfer succeeded regardless of ping state
**Status**: Successfully implemented and tested - device now works perfectly!

### Solution 2: Disable PING Protocol
**Rationale**: Prevent ping protocol issues entirely
**Status**: Not needed - Solution 1 completely resolved the issue

### Solution 3: Force Full Speed Mode  
**Rationale**: Full Speed doesn't use ping protocol
**Status**: Not needed - device works at full High Speed (480 Mbps) with Solution 1

## Final Solution

### Root Cause
The Razer Viper V3 Pro wireless dongle uses unique USB High Speed ping protocol timing that causes false positive errors. The USB controller successfully transfers data, but incorrectly sets the Ping State bit (bit 0) in the status byte, causing our driver to reject valid transfers.

### The Fix
Modified `USBHostDriver::control()` to accept transfers when only the Ping State bit is set, for both IN and OUT transfers:

```cpp
// SOLUTION 1: Accept transfers despite ping error
// This handles the Razer Viper V3 Pro wireless false positive
// Handle both IN transfers with data and OUT transfers
if (status == 0x01) {
    // Only Ping bit set - check transfer type
    uint8_t pid_code = (token >> 8) & 0x03;
    
    // IN transfer with data received
    if (control_length_received > 0) {
        control_success = true;
        logger.debug("WORKAROUND: IN transfer - Treating Ping-only error as success since data was received");
        logger.debugf("Token=0x%08X, Status=0x01 (Ping only), Length=%d bytes", 
                     token, control_length_received);
    }
    // OUT transfer (PID=1) with ping error  
    else if (pid_code == 1) {
        control_success = true;
        logger.debug("WORKAROUND: OUT transfer - Treating Ping-only error as success for SET_CONFIGURATION");
        logger.debugf("Token=0x%08X, Status=0x01 (Ping only), PID=%d (OUT)", token, pid_code);
    }
    else {
        // Some other case with ping bit set
        control_success = false;
        logger.debugf("Ping error without data or OUT transfer, token=0x%08X", token);
    }
} else {
    // Normal success check - status byte should be 0
    control_success = (status == 0);
}
```

## Verification Results

### Test Date: 2025-01-20

1. **All Control Transfers Succeed**:
   - HID descriptor retrieval: ✅ Success with workaround
   - Device descriptor: ✅ Success with workaround  
   - Configuration descriptor: ✅ Success with workaround
   - String descriptors: ✅ Success with workaround
   - SET_CONFIGURATION: ✅ Success with workaround (token=0x00008101)
   - SET_REPORT commands: ✅ Success with workaround

2. **Windows Enumeration**: ✅ Complete
   - Device recognized as "Razer Viper V3 Pro"
   - All HID interfaces properly configured
   - No driver errors in Windows

3. **Mouse Operation**: ✅ Fully Functional
   - Mouse data packets flowing at 8kHz
   - Button clicks working
   - Movement tracking working
   - Device fully operational at High Speed (480 Mbps)

### Sample Log Output
```
D: WORKAROUND: IN transfer - Treating Ping-only error as success since data was received
D: Token=0x00008001, Status=0x01 (Ping only), Length=94 bytes

D: WORKAROUND: OUT transfer - Treating Ping-only error as success for SET_CONFIGURATION
D: Token=0x00008101, Status=0x01 (Ping only), PID=1 (OUT)

S: Received first mouse data packet, length: 8, data: 01 00 00 00 00 00 00 00, device should now be fully operational.
```

## Implementation Progress

### ✅ Completed
- Enhanced debug logging added
- Root cause identified: Ping protocol false positive
- Valid data confirmed in failed transfers
- Solution 1 implemented for IN transfers
- Solution 1 extended for OUT transfers
- Full device functionality verified
- Windows compatibility confirmed

### 🎉 Issue Resolved
The Razer Viper V3 Pro wireless dongle now works perfectly with the USB Host Proxy!

## Updates
*2025-01-20*: Solution 1 fully implemented and verified. Device works perfectly at High Speed (480 Mbps) with full 8kHz polling rate support. No further fixes needed.