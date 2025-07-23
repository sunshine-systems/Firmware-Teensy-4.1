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