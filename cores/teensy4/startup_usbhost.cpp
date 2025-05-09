#include "startup_usbhost.h" // Our header
#include "imxrt.h"           // Include for direct register access (USB2_PORTSC1, USB_PORTSC1_PP)
#include <string.h>          // For memset, strncpy
#include "debug/printf.h"     // For printf()

// Global variable definition
usb_proxy_info_t g_proxy_info = {0};

// Function to populate static data (NO CHANGES NEEDED HERE)
extern "C" int startup_populate_static_spoof_data(void) {
    printf(">>> SUH: Populating static spoof data into g_proxy_info...\n");
    memset(&g_proxy_info, 0, sizeof(g_proxy_info));
    // --- Hardcode your desired spoofed values ---
    g_proxy_info.idVendor = 0x1209; g_proxy_info.idProduct = 0xDAF7; g_proxy_info.bcdDevice = 0x0100;
    g_proxy_info.bDeviceClass = 0xEF; g_proxy_info.bDeviceSubClass = 0x02; g_proxy_info.bDeviceProtocol = 0x01;
    g_proxy_info.bMaxPacketSize0 = 64;
    strncpy(g_proxy_info.manufacturerString, "SunshineMfg", MAX_SPOOF_STRING_LEN);
    strncpy(g_proxy_info.productString, "Sunshine Proxy KbdMouse", MAX_SPOOF_STRING_LEN);
    strncpy(g_proxy_info.serialNumberString, "SPKM001", MAX_SPOOF_STRING_LEN);
    g_proxy_info.manufacturerString[MAX_SPOOF_STRING_LEN] = '\0';
    g_proxy_info.productString[MAX_SPOOF_STRING_LEN] = '\0';
    g_proxy_info.serialNumberString[MAX_SPOOF_STRING_LEN] = '\0';
    g_proxy_info.valid = 1;
    printf("    SUH: Static Spoof Data Populated (details omitted for brevity)\n");
    printf("<<< SUH: Static data population complete.\n");
    return 0; // Success
}

// --- NEW C-Callable Hook Function ---
// Called from startup.c AFTER C++ constructors (__libc_init_array)
// This function will populate spoof data AND set the Host Port Power bit.
extern "C" int startup_power_on_host_port(void) {
    printf(">>> SUH: startup_power_on_host_port called.\n");

    // --- Step 1: Populate static spoof data (for Device Port) ---
    int spoof_result = startup_populate_static_spoof_data();
    if (spoof_result != 0) {
        printf("SUH: ERROR - Failed to populate static spoof data.\n");
        // Continue anyway, maybe power is still useful
    } else {
        printf("SUH: Static spoof data populated successfully.\n");
    }

    // --- Step 2: Power on USB Host Port (USB2) ---
    // Assumes usb_host_phy_pll_start() in startup.c has already run successfully
    // and left the controller reset but ready.
    printf("SUH: Attempting to enable USB Host Port 2 Power (PORTSC1[PP])...\n");

    // Check current state (optional debug)
    printf("  SUH: Current USB2_PORTSC1 = 0x%08lX\n", USB2_PORTSC1);

    // Set the Port Power bit
    // Ensure USB_PORTSC1_PP is defined correctly in imxrt.h (it usually is)
    #ifndef USB_PORTSC1_PP
    #define USB_PORTSC1_PP (1U << 12) // Define if missing
    #endif
    USB2_PORTSC1 |= USB_PORTSC1_PP;

    // Add a small delay to allow power to stabilize
    for(volatile int d=0; d<500; d++) { asm volatile ("nop"); }

    // Check state again (optional debug)
    printf("  SUH: USB2_PORTSC1 after setting PP = 0x%08lX\n", USB2_PORTSC1);

    if (USB2_PORTSC1 & USB_PORTSC1_PP) {
        printf("  SUH: USB Host Port 2 Power bit (PP) is SET.\n");
    } else {
        printf("  SUH: WARN - USB Host Port 2 Power bit (PP) failed to set!\n");
        // This would indicate a problem, potentially the controller wasn't ready
        // even though usb_host_phy_pll_start completed.
    }

    // --- Step 3: DEFERRED ---
    // NO USB Host library initialization (myusb.begin()) here.
    // This will be done by the main sketch's setup().
    printf("SUH: USB Host library init (myusb.begin()) deferred to sketch setup().\n");

    printf("<<< SUH: startup_power_on_host_port finished.\n");
    return 0; // Indicate success
}
// --- END NEW C-Callable Hook Function ---