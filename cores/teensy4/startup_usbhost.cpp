#include "startup_usbhost.h" // Our header
#include <string.h>          // For strncpy
#include "debug/printf.h"     // For printf(), output to Serial4

// Global variable definition
usb_proxy_info_t g_proxy_info = {0};

// This function now *only* populates g_proxy_info with hardcoded static data.
// It does NOT attempt any USB Host operations.
extern "C" int startup_populate_static_spoof_data(void) {
    printf(">>> SUH: Populating static spoof data into g_proxy_info...\n");

    memset(&g_proxy_info, 0, sizeof(g_proxy_info));

    // --- Hardcode your desired spoofed values ---
    g_proxy_info.idVendor = 0x1209;         // Example: "Generic" vendor (PID.codes)
    g_proxy_info.idProduct = 0xDAF7;        // Example: "Sunshine Proxy Device" (make this unique)
    g_proxy_info.bcdDevice = 0x0100;        // Device version 1.00

    // For a composite device using IAD, or if class is per-interface (like RawHID + other HID):
    g_proxy_info.bDeviceClass = 0xEF;       // Miscellaneous Device Class
    g_proxy_info.bDeviceSubClass = 0x02;    // Common Subclass
    g_proxy_info.bDeviceProtocol = 0x01;    // Interface Association Descriptor Protocol
    // If you wanted to appear as a single HID device (e.g. just keyboard):
    // g_proxy_info.bDeviceClass = 0x03;    // HID Class
    // g_proxy_info.bDeviceSubClass = 0x00; // No subclass or 0x01 for Boot Interface
    // g_proxy_info.bDeviceProtocol = 0x00; // No protocol or 0x01 for Keyboard / 0x02 for Mouse

    g_proxy_info.bMaxPacketSize0 = 64;      // Standard for Full/High speed

    strncpy(g_proxy_info.manufacturerString, "SunshineMfg", MAX_SPOOF_STRING_LEN);
    strncpy(g_proxy_info.productString, "Sunshine Proxy KbdMouse", MAX_SPOOF_STRING_LEN);
    strncpy(g_proxy_info.serialNumberString, "SPKM001", MAX_SPOOF_STRING_LEN);
    
    // Ensure null termination
    g_proxy_info.manufacturerString[MAX_SPOOF_STRING_LEN] = '\0';
    g_proxy_info.productString[MAX_SPOOF_STRING_LEN] = '\0';
    g_proxy_info.serialNumberString[MAX_SPOOF_STRING_LEN] = '\0';

    // For this test, we are NOT spoofing a specific HID report descriptor via g_proxy_info.
    // The Teensy will still present its *own* compiled-in configuration descriptor
    // (e.g., for RawHID, which has its own HID report descriptor).
    // Modifying that entire structure at runtime is much more complex.
    // We are only changing Device Descriptor and String Descriptors initially.

    g_proxy_info.valid = 1; // Mark data as ready

    printf("    SUH: Static Spoof Data Populated:\n");
    printf("        VID: 0x%04X, PID: 0x%04X, bcdDev: 0x%04X\n", 
           g_proxy_info.idVendor, g_proxy_info.idProduct, g_proxy_info.bcdDevice);
    printf("        Class:0x%02X, Sub:0x%02X, Proto:0x%02X, MPS0:%u\n",
           g_proxy_info.bDeviceClass, g_proxy_info.bDeviceSubClass, g_proxy_info.bDeviceProtocol, g_proxy_info.bMaxPacketSize0);
    printf("        Mfr: '%s'\n", g_proxy_info.manufacturerString);
    printf("        Prd: '%s'\n", g_proxy_info.productString);
    printf("        Ser: '%s'\n", g_proxy_info.serialNumberString);
    printf("<<< SUH: Static data population complete.\n");

    return 0; // Success
}