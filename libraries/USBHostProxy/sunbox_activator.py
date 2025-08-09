#!/usr/bin/env python3
"""
SunBox Activator - Authorization Key Generator for USB Host Proxy
Generates activation codes for Teensy devices running USB Host Proxy firmware
"""

import sys
import argparse

# Secret key used for XOR operation (must match firmware)
SECRET_KEY = 0xCAFEBABE87654321

def validate_hex_input(hex_string):
    """Validate that input is a 16-character hexadecimal string."""
    # Remove any spaces or common separators
    cleaned = hex_string.strip().replace(" ", "").replace(":", "").upper()
    
    # Check length
    if len(cleaned) != 16:
        return None, f"Invalid length: {len(cleaned)} characters (expected 16)"
    
    # Check if all characters are valid hex
    try:
        int(cleaned, 16)
    except ValueError:
        return None, "Invalid characters - must be hexadecimal (0-9, A-F)"
    
    return cleaned, None

def calculate_auth_key(hardware_id_str):
    """Calculate the authorization key for a given hardware ID."""
    # Convert string to integer
    hw_id = int(hardware_id_str, 16)
    
    # XOR with secret key
    auth_key = hw_id ^ SECRET_KEY
    
    return auth_key

def format_auth_key(auth_key):
    """Format the authorization key as a 16-character hex string."""
    return f"{auth_key:016X}"

def main():
    parser = argparse.ArgumentParser(
        description='Generate authorization keys for SunBox USB Host Proxy devices',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s                    Interactive mode
  %(prog)s -i 6E21BFB10B0B66F5  Direct calculation
  %(prog)s --verbose         Show calculation details
        """
    )
    
    parser.add_argument('-i', '--id', 
                       help='Hardware ID (16 hex chars) for direct calculation')
    parser.add_argument('-v', '--verbose', 
                       action='store_true',
                       help='Show detailed calculation steps')
    
    args = parser.parse_args()
    
    print("=" * 60)
    print("SunBox USB Host Proxy - Authorization Key Generator")
    print("=" * 60)
    print()
    
    # Get hardware ID either from argument or interactively
    if args.id:
        hw_id_str, error = validate_hex_input(args.id)
        if error:
            print(f"❌ Error: {error}")
            sys.exit(1)
    else:
        # Interactive mode
        print("📋 STEP 1: Get the Hardware ID")
        print("-" * 40)
        print("1. Connect to your device via Serial4 (115200 baud)")
        print("2. Send this command: pwrdiag")
        print("3. You will receive a 16-character hex response")
        print()
        
        while True:
            hw_id_input = input("📝 Enter the Hardware ID received: ").strip()
            
            if not hw_id_input:
                print("❌ No input provided. Exiting.")
                sys.exit(1)
            
            hw_id_str, error = validate_hex_input(hw_id_input)
            if error:
                print(f"❌ Error: {error}")
                retry = input("Try again? (y/n): ").lower()
                if retry != 'y':
                    sys.exit(1)
            else:
                break
    
    print()
    print("✅ Hardware ID validated:", hw_id_str)
    
    # Calculate authorization key
    auth_key = calculate_auth_key(hw_id_str)
    auth_key_str = format_auth_key(auth_key)
    
    if args.verbose:
        print()
        print("🔧 CALCULATION DETAILS:")
        print("-" * 40)
        print(f"Hardware ID:     0x{hw_id_str}")
        print(f"Secret Key:      0x{SECRET_KEY:016X}")
        print(f"XOR Result:      0x{auth_key_str}")
        
        # Show byte-by-byte breakdown
        hw_bytes = [hw_id_str[i:i+2] for i in range(0, 16, 2)]
        secret_bytes = [f"{(SECRET_KEY >> (56 - i*8)) & 0xFF:02X}" for i in range(8)]
        result_bytes = [auth_key_str[i:i+2] for i in range(0, 16, 2)]
        
        print()
        print("Byte-by-byte XOR:")
        for i in range(8):
            print(f"  {hw_bytes[i]} XOR {secret_bytes[i]} = {result_bytes[i]}")
    
    print()
    print("=" * 60)
    print("🔑 ACTIVATION COMMAND")
    print("=" * 60)
    print()
    print("Copy and send this EXACT command to your device:")
    print()
    print(f"  pwroverride {auth_key_str}")
    print()
    print("After sending, you will see: 'Power cycle required.'")
    print("Power cycle your device to complete activation.")
    print()
    
    # Additional instructions
    print("📌 NOTES:")
    print("-" * 40)
    print("• The authorization persists across power cycles")
    print("• To deauthorize, send: pwrreset")
    print("• Each device needs its own unique authorization key")
    print()
    
    return 0

if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n\n❌ Cancelled by user")
        sys.exit(1)
    except Exception as e:
        print(f"\n❌ Unexpected error: {e}")
        sys.exit(1)