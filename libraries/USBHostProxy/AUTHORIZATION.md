# USB Host Proxy Authorization System

## Overview

The USB Host Proxy firmware includes a hardware-based authorization system that prevents unauthorized use of the firmware. Each Teensy device must be individually authorized using its unique hardware ID before the USB proxy functionality will activate.

## System Architecture

### Authorization Flow

1. **Initial Boot (Unauthorized)**
   - Serial4 initializes (115200 baud)
   - Authorization check fails
   - USB proxy remains disabled
   - Device listens for activation commands only

2. **Activation Process**
   - User sends `sunbox` command to get hardware ID
   - External tool generates authorization key
   - User sends `moonrise` command with key
   - Device stores authorization in EEPROM
   - Power cycle required to activate

3. **Normal Operation (Authorized)**
   - Serial4 initializes
   - Authorization check passes
   - Full USB proxy functionality activates
   - All normal commands available

## Hidden Commands

These commands are not documented in the help menu and only work via Serial4. They are disguised as power management commands to match the "power fault" error message:

### `pwrdiag`
Power diagnostics - Returns the device's unique hardware identifier as a 16-character hexadecimal string.

**Example:**
```
> pwrdiag
< A5B3C7D9E1F24680
```

### `pwroverride <code>`
Power override code - Activates the device with the provided authorization code.

**Format:** `pwroverride XXXXXXXXXXXXXXXX`
- Code must be exactly 16 hexadecimal characters
- No spaces or separators
- Case-insensitive

**Example:**
```
> pwroverride 1234567890ABCDEF
< Power cycle required.
```

### `pwrreset`
Power reset - Removes the current authorization (for testing/deauthorization).

**Example:**
```
> pwrreset
< Power cycle required.
```

## Hidden DevTools Command

This command is available through the normal DevTools interface when the device is authorized and running:

### `pwrclear`
Hidden command that clears authorization from EEPROM. Unlike `pwrreset` which only works in unauthorized mode, this command works when the device is fully operational.

**Availability:** Only when device is authorized and running normally  
**Visibility:** Not shown in help menu (completely hidden)  
**Access:** Through normal serial commands (not the auth-only interface)

**Example:**
```
> pwrclear
< Power override cleared. Power cycle required.
```

**Use Cases:**
- Testing authorization system without needing unauthorized access
- Recovery if auth commands are not accessible
- Development and debugging

**Note:** This is a backdoor command for testing/recovery. The primary deauthorization method for unauthorized devices is `pwrreset`.

## Technical Details

### Hardware ID Generation

The hardware ID is derived from the Teensy's unique OCOTP (On-Chip One-Time Programmable) registers:

1. Read `HW_OCOTP_MAC0` (32 bits) and `HW_OCOTP_MAC1` (32 bits)
2. Combine into 64-bit value: `id = (MAC1 << 32) | MAC0`
3. XOR with pattern: `0xDEADC0DE13371337`
4. Rotate left by `(MAC0 & 0x0F)` bits
5. Result is 8-byte hardware ID

### Authorization Key Algorithm

The authorization key is generated from the hardware ID:

1. Take 8-byte hardware ID
2. XOR with secret: `0xCAFEBABE87654321`
3. Calculate checksum: sum of all bytes (32-bit)
4. Final key = XOR result (8 bytes) + checksum (4 bytes)

### EEPROM Storage

Authorization data is stored at EEPROM address `0x20`:

```c
struct AuthConfig {
    uint32_t magic;      // 0x53554E42 ('SUNB')
    uint64_t deviceId;   // Processed hardware ID
    uint32_t authKey;    // Authorization key checksum
    uint32_t checksum;   // Data validation
};
```

Total size: 20 bytes

## Manual Key Generation

For testing purposes, you can manually calculate an authorization key:

### Step 1: Get Hardware ID
```
Send: pwrdiag
Receive: A5B3C7D9E1F24680  (example)
```

### Step 2: Convert and Process
```python
# Example Python calculation
hw_id = 0xA5B3C7D9E1F24680
secret = 0xCAFEBABE87654321
result = hw_id ^ secret

# Calculate checksum (sum of bytes)
checksum = 0
temp = result
for i in range(8):
    checksum += temp & 0xFF
    temp >>= 8
checksum &= 0xFFFFFFFF

# Format as hex string
auth_key = f"{result:016X}"
```

### Step 3: Activate Device
```
Send: pwroverride <calculated_key>
```

## Testing Procedure

### Test 1: Unauthorized State
1. Upload firmware to Teensy
2. Connect via Serial4 (115200 baud)
3. Verify USB device does NOT enumerate
4. Send `help` - no response expected
5. Send `status` - no response expected
6. Send `pwrdiag` - should return hardware ID

### Test 2: Authorization
1. Get hardware ID with `pwrdiag`
2. Calculate or generate authorization key
3. Send `pwroverride <key>`
4. Power cycle device
5. Verify USB device now enumerates
6. Verify all normal commands work

### Test 3: Deauthorization
1. Send `pwrreset` command
2. Power cycle device
3. Verify device returns to unauthorized state
4. Verify USB does not enumerate

## Troubleshooting

### Device Not Responding
- Verify Serial4 connection (TX: Pin 17, RX: Pin 16)
- Check baud rate is 115200
- Ensure proper power supply

### Authorization Not Persisting
- Check EEPROM is not corrupted
- Verify power cycle after authorization
- Try `eclipse` then re-authorize

### Wrong Hardware ID
- Hardware ID is unique per chip
- Cannot be changed or spoofed
- Each device needs individual authorization

## Security Notes

⚠️ **Important:** This is a basic protection mechanism designed to prevent casual firmware sharing. It is NOT cryptographically secure and should not be relied upon for high-security applications.

- Hardware ID cannot be changed (burned in OTP memory)
- Authorization survives power cycles and reprogramming
- Serial4 always remains active for recovery
- No error messages reveal authorization state
- Silent failure prevents information leakage

## Development Tools

### Future: Authorization Key Generator
A separate tool will be developed to:
- Connect to unauthorized devices
- Read hardware IDs
- Generate proper authorization keys
- Batch authorize multiple devices
- Manage authorization database

### Manual Testing Script
```bash
# Example bash script for testing
echo "pwrdiag" > /dev/ttyUSB0
sleep 1
# Read response and calculate key
echo "pwroverride 1234567890ABCDEF" > /dev/ttyUSB0
```

## Implementation Files

- `SunBoxAuth.cpp/h` - Core authorization logic
- `SunBoxEEPROM.cpp/h` - EEPROM storage functions
- `SunBoxStartup.cpp/h` - Startup sequence integration
- `startup.c` - Early boot authorization check

## Version History

- v1.0 - Initial authorization system implementation
- Hidden commands: pwrdiag, pwroverride, pwrreset
- EEPROM-based persistence
- Hardware ID from OCOTP registers