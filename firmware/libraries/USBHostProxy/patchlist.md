# USBHostProxy Patch List

This document tracks issues discovered and fixes applied to the USBHostProxy library.

---

## 2025-07-23: HID Parser Report ID Offset Bug

**Issue:** M: messages were printing mouse movement data (X/Y values) instead of button values for devices that use Report IDs. After initial fix, mouse movement stopped working entirely.

**Affected Device:** Endgame Gear HS Dongle (VID: 0x3367, PID: 0x1970)

**Symptoms:**
- When moving mouse right: `M: 05` (X movement value)
- When moving mouse up: `M: 01` (Y movement value)  
- When moving mouse left/down: `M: FF`, `M: FE`, etc. (negative movement values)
- After partial fix: Button detection worked but mouse movement stopped

**Root Cause:** 
The HID descriptor parser correctly calculates bit offsets including the Report ID byte. However, when processing actual data, the code skips the Report ID byte but wasn't adjusting the bit offsets accordingly. This affected both parsing (reading) and formatting (writing) of mouse data.

Example with Report ID 0x01:
- Raw data: `[0x01][0x00][0x05][0x00][0x01][0x00][0x00][0x00]`
- After skipping Report ID: `[0x00][0x05][0x00][0x01][0x00][0x00][0x00]`
- Button field has bit_offset=8 (byte 1), but after skipping Report ID, byte 1 is now X movement!

**Fix Applied:** 
1. Modified `HIDMouseDescriptorHandler::parseMouseData()` to adjust bit offsets by 8 bits when Report ID is skipped
2. Modified `HIDMouseDescriptorHandler::formatMouseData()` to apply the same offset adjustment when formatting data
3. Removed temporary workaround that was overwriting the first byte with button data

**Files Modified:**
- `src/HIDMouseDescriptorHandler.cpp` - Both parseMouseData() and formatMouseData() functions
- `examples/SunshineUSBProxy/SunBoxSyntheticHandleOutput.cpp` - Removed temporary fix

---

## 2025-07-23: Report ID to Boot Protocol Translation Issue

**Issue:** All mouse button presses were being processed by Windows as left clicks, even though logging showed correct button values (0x02 for right click, 0x04 for middle click, etc.)

**Affected Device:** Endgame Gear HS Dongle (VID: 0x3367, PID: 0x1970)

**Symptoms:**
- Right click (0x02) registered as left click in Windows
- Middle click (0x04) registered as left click in Windows
- Side buttons (0x08, 0x10) registered as left click in Windows

**Root Cause:**
The Teensy USB device interface presents itself to Windows as a boot protocol mouse (no Report ID), but the Endgame Gear mouse sends data with Report ID 0x01. When forwarding data to Windows, the Report ID byte needs to be stripped to match the boot protocol format expected by Windows.

**Fix Applied:**
Modified `SunBoxSyntheticHandleOutput::process()` to detect when source device uses Report ID and strip it before sending to Windows. Added `hasReportId()` method to HIDMouseDescriptorHandler for detection.

**Files Modified:**
- `examples/SunshineUSBProxy/SunBoxSyntheticHandleOutput.cpp` - Added Report ID stripping logic
- `src/HIDMouseDescriptorHandler.h` - Added hasReportId() method

**Update:** The Report ID stripping approach was incorrect. The proper fix was to remove offset adjustment from `formatMouseData`.

**Root Cause Analysis:**
The `formatMouseData` function was incorrectly adjusting bit offsets when a Report ID was present. This caused a double-adjustment:
1. The stored field offsets already account for Report ID position (e.g., buttons at bit 8 for Report ID devices)
2. `formatMouseData` was then subtracting 8 from these offsets, placing data at wrong positions

**Permanent Fix Applied:**
Removed the offset adjustment logic from `formatMouseData`. The function now uses the field bit offsets directly as stored by the HID parser. This ensures:
- Devices with Report ID format data as: `[ReportID][Buttons][X][Y]...`
- Devices without Report ID format data as: `[Buttons][X][Y]...`
- The Teensy faithfully emulates whatever format the connected device uses

**Additional Files Modified:**
- `src/HIDMouseDescriptorHandler.cpp` - Removed offset_adjustment from formatMouseData()
- `examples/SunshineUSBProxy/SunBoxSyntheticHandleOutput.cpp` - Removed temporary button override

---

## 2025-07-23: Dynamic Button Positioning Fix

**Issue:** All mouse button presses were being interpreted as left clicks by Windows, even though the M: logs showed correct button values (0x02 for right, 0x04 for middle, etc.)

**Affected Device:** Endgame Gear HS Dongle (VID: 0x3367, PID: 0x1970)

**Symptoms:**
- M: logs showed correct button values (0x01, 0x02, 0x04, 0x08, 0x10)
- Windows interpreted all buttons as left clicks
- Mouse movement worked correctly

**Root Cause:**
The `formatMouseData` function was not correctly placing button data at the expected byte position. The HID parser knows where buttons should be placed based on the descriptor, but the formatting wasn't working properly.

**Fix Applied:**
Added dynamic button positioning based on HID descriptor:
- Added `getButtonByteOffset()` method to get correct button position from HID parser
- Modified `SunBoxSyntheticHandleOutput::process()` to place buttons at the correct byte
- Works for any device format (with or without Report ID)
- Only overrides button byte if `formatMouseData` didn't place it correctly
- Added debug logging to help diagnose formatMouseData issues

**Files Modified:**
- `src/HIDMouseDescriptorHandler.h` - Added getButtonByteOffset() method
- `examples/SunshineUSBProxy/SunBoxSyntheticHandleOutput.cpp` - Dynamic button positioning with debug logging

---

## 2026-03-06: Enumeration Timing Optimization — Delay Removal

**Issue:** USB pass-through proxy had ~315ms enumeration time vs ~92ms for direct USB connection, making it detectable via Kernel-PnP event timing analysis.

**Root Cause:** Multiple unnecessary `delay()` calls in the control transfer path:
- `delay(10)` per string descriptor request in USBDeviceProxy.cpp (~30-40ms total)
- `delay(5)` per control transfer in USBHostDriver::controlTransfer() (~40-70ms total, fired unconditionally on every transfer)
- `delay(5)` in USBHostDriver::pauseDataTransfers() (~5ms)
- `delayMicroseconds(100)` in USBHostDriver::control() callback (~1ms)

**Fix Applied:**
1. Removed all four delay sources
2. Cleaned up unused variables (xfer_duration, xfer_start, proxy_endpoint0_buffer)

**Impact:** Enumeration time reduced from ~315ms to ~120ms (62% reduction, 87% overhead reduction vs direct USB)

**Files Modified:**
- `src/USBDeviceProxy.cpp` — Removed delay(10) for string descriptors, removed unused variables
- `src/USBHostDriver.cpp` — Removed delay(5) in controlTransfer(), delay(5) in pauseDataTransfers(), delayMicroseconds(100) in control callback

---

## 2026-03-06: USB Descriptor Caching

**Issue:** During enumeration, Windows requests the same descriptors multiple times (short probe then full fetch). Each request required a synchronous USB round-trip through the proxy to the physical device.

**Fix Applied:**
- Added DescriptorCache struct to USBDeviceProxy with static allocation (~3.6KB)
- Caches device (0x01), configuration (0x02), string (0x03 x10), and BOS (0x0F) descriptors
- Config/BOS only cached after full fetch (not short probe) to handle two-phase Windows enumeration
- Cache invalidated on device disconnect and VID/PID change
- Bidirectional reference between USBDeviceProxy and USBHostDriver for cache invalidation

**Impact:** Eliminates redundant USB round-trips during re-enumeration (~7ms saved)

**Files Modified:**
- `src/USBDeviceProxy.cpp` — Cache lookup before controlTransfer(), cache store after fetch
- `src/USBDeviceProxy.h` — DescriptorCache struct, cache methods
- `src/USBHostDriver.cpp` — Cache invalidation on disconnect
- `src/USBHostDriver.h` — Forward declaration, setDeviceProxy() method
- `examples/SunshineUSBProxy/SunshineUSBProxy.ino` — Wire up bidirectional reference

---

## 2026-03-07: Logging Channel System with Compile-Time Toggle

**Issue:** Debug logging in the control transfer hot path caused a ~50ms enumeration regression (B6: 167ms vs B4: 119ms). Need a way to have logging available for development but completely stripped for production.

**Fix Applied:**
- Added LogChannel enum with bitmask channels: BOOT(0x01), CONNECT(0x02), ENUM(0x04), DATA(0x08), COMMAND(0x10), STATS(0x20), ERROR(0x40)
- All ~313 non-error log calls in src/ files tagged with channel macros (LOG_BOOT, LOG_CONNECT, etc.)
- Channel mask persisted to EEPROM at offset 0x40 with magic validation
- Default mask: ERROR-only (minimal output)
- Added `#define SUNBOX_LOGGING 1` compile-time toggle in SunBoxLogger.h
- When SUNBOX_LOGGING=0: all logging stripped at compile time, zero overhead
- Example files (runtime code) NOT modified per design

**Impact:** Zero performance regression with logging disabled. Full diagnostic capability when enabled.

**Files Modified:**
- `src/SunBoxLogger.h` — Compile toggle, LogChannel enum, channel macros, stripped class for SUNBOX_LOGGING=0
- `src/SunBoxLogger.cpp` — Channel methods wrapped in #if guard
- `src/SunBoxEEPROM.h` — Log channel config struct and addresses
- `src/SunBoxEEPROM.cpp` — Read/write log channel mask, wrapped in #if guard
- `src/USBDeviceProxy.cpp` — ~55 calls tagged with channel macros
- `src/USBHostDriver.cpp` — ~68 calls tagged with channel macros
- `src/HIDMouseDescriptorHandler.cpp` — ~46 calls tagged
- `src/SunBoxStartup.cpp` — 2 calls tagged

---

## 2026-03-22: Control Transfer Timing Fix — Inter-Transfer Throttle + SET_CONFIG Settle

**Issue:** Removing all control transfer delays (2026-03-06) broke HID interface initialization and vendor software communication. All 3 HID interfaces failed to start (EventID 411 "had a problem starting"), causing a 4.4-second Windows retry cycle. Vendor compliance software (Razer Synapse, Logitech G HUB, etc.) could not communicate with devices because rapid-fire class/vendor control transfers overwhelmed the device before it had time to recover.

**Root Cause:** Two separate timing issues:
1. **Post-configuration settle time**: After SET_CONFIGURATION, the device needs time to initialize its HID interfaces before the host sends class-specific requests (GET_REPORT_DESCRIPTOR, SET_IDLE). Without the original `delay(5)` per transfer, these requests arrived before interfaces were ready.
2. **Inter-transfer recovery**: Vendor software rapid-fires control transfers at runtime. The device needs brief recovery time between consecutive transfers.

**Fix Applied:**
1. **Inter-transfer throttle** (`USBHostDriver`): Tracks `lastControlTransferCompleteUs` timestamp. Before each new control transfer, if less than `CONTROL_TRANSFER_MIN_GAP_US` (800µs) has elapsed since the last completion, delays only the remaining difference. Updated on all completion paths (success, failure, timeout). First transfer always passes with zero delay. During enumeration, host pacing naturally spaces transfers >800µs apart, so this adds near-zero overhead.
2. **Post-SET_CONFIGURATION settle delay** (`USBDeviceProxy`): `SET_CONFIG_SETTLE_MS` (5ms) delay after SET_CONFIGURATION completes (after endpoint setup and `resumeDataTransfers()`). Fires once per enumeration. Gives the device time to initialize HID interfaces before Windows sends class-specific requests.

**Impact:** HID interfaces start cleanly on first attempt (zero EventID 411 errors). Vendor software communicates successfully. Enumeration time ~197ms (up from ~120ms but down from 4.6s failure case). Previous ~120ms was non-functional.

**Files Modified:**
- `src/USBHostDriver.h` — Added `CONTROL_TRANSFER_MIN_GAP_US` constant (800µs), `lastControlTransferCompleteUs` member
- `src/USBHostDriver.cpp` — Throttle logic at start of `controlTransfer()`, timestamp updates in `control()` callback, queue failure path, and timeout path
- `src/USBDeviceProxy.cpp` — Added `SET_CONFIG_SETTLE_MS` constant (5ms), `delay()` after SET_CONFIGURATION handling

---

## 2026-03-08: Movement Sanitization System

**Issue:** Combined USB + serial mouse output created detectable signatures: polling rate inflation from extra serial-only frames, rapid direction reversals (sign flips) when serial fought USB direction, single-frame value spikes from async serial deltas, and instant velocity step-functions from sensitivity reduction toggling.

**Fix Applied:**
- Added `MovementProfileTracker` with 32-entry ring buffer tracking real USB movement to derive adaptive thresholds (avgSpeed, avgAccel, maxDelta, USB polling rate)
- **Output Rate Regulation**: Serial-only frames throttled when output rate exceeds real USB rate + 10%. Excess deltas accumulated and smoothly released (capped per frame at adaptive threshold, decayed after 50ms)
- **Sign Flip Limiter**: Direction reversals capped at 8/sec per axis. When exceeded and serial caused the flip, reverts to USB-only value
- **Adaptive Spike Clamper**: Frame-to-frame output jumps capped at `max(3, avgSpeed + avgAccel * 2)`. Adapts to user's real movement — fast movement = permissive, slow = tight
- **Sensitivity Reduction Ramp**: 32ms linear interpolation replaces instant step-function for lockout activation/deactivation
- All thresholds hardcoded, no serial configuration needed

**Impact:** Output movement follows natural acceleration curves matching the user's real input. No detectable polling rate inflation, sign flip anomalies, value spikes, or velocity discontinuities.

**Files Modified:**
- `examples/SunshineUSBProxy/SunBoxSyntheticHandleOutput.h` — Added structs, constants, members, method declarations
- `examples/SunshineUSBProxy/SunBoxSyntheticHandleOutput.cpp` — Implemented all 4 safeguards
- `examples/SunshineUSBProxy/MOVEMENT_SANITIZATION.md` — Technical documentation

---