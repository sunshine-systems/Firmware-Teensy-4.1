// --- START OF FILE startup_usbhost.cpp ---
#include "startup_usbhost.h"
#include "imxrt.h"        // For low-level register access if needed elsewhere
#include <Arduino.h>       // To ensure HardwareSerial and Serial1 are available
#include "USBHost_t36.h"
#include <string.h>
#include "debug/printf.h"  // Your existing debug printf

usb_proxy_info_t g_proxy_info = {0};
USBHost myusb;

extern "C" int startup_populate_static_spoof_data(void) {
    // ... (implementation as before) ...
    printf(">>> SUH: Populating static spoof data into g_proxy_info...\n");
    memset(&g_proxy_info, 0, sizeof(g_proxy_info));
    g_proxy_info.idVendor = 0x1209;
    g_proxy_info.idProduct = 0xDAF7;
    g_proxy_info.bcdDevice = 0x0100;
    g_proxy_info.bDeviceClass = 0xEF;
    g_proxy_info.bDeviceSubClass = 0x02;
    g_proxy_info.bDeviceProtocol = 0x01;
    g_proxy_info.bMaxPacketSize0 = 64;
    strncpy(g_proxy_info.manufacturerString, "SunshineMfg", MAX_SPOOF_STRING_LEN);
    g_proxy_info.manufacturerString[MAX_SPOOF_STRING_LEN] = '\0';
    strncpy(g_proxy_info.productString, "Sunshine Proxy KbdMouse", MAX_SPOOF_STRING_LEN);
    g_proxy_info.productString[MAX_SPOOF_STRING_LEN] = '\0';
    strncpy(g_proxy_info.serialNumberString, "SPKM001", MAX_SPOOF_STRING_LEN);
    g_proxy_info.serialNumberString[MAX_SPOOF_STRING_LEN] = '\0';
    g_proxy_info.valid = 1;
    printf("    SUH: Static Spoof Data Populated:\n");
    printf("        VID: 0x%04X, PID: 0x%04X, bcdDev: 0x%04X\n",
           g_proxy_info.idVendor, g_proxy_info.idProduct, g_proxy_info.bcdDevice);
    printf("<<< SUH: Static data population complete.\n");
    return 0;
}

extern "C" int startup_host_init_and_spoof(void) {
    // This printf goes to your existing debug output (Serial4, pins 16/17)
    printf(">>> SUH: startup_host_init_and_spoof called.\n");

    // Initialize Serial1 (LPUART6, typically pins 0/1 on T4.1) for USBHost_t36 library logs
    // USBHost_t36.h should have USBHDBGSerial defined as Serial1
    // and #define USBHOST_PRINT_DEBUG uncommented.
    Serial1.begin(115200); 
    // A small delay might help ensure the UART is fully ready, though often not strictly necessary.
    // You might need to experiment if the very first log messages from USBHost_t36 are missed.
    // delay(1); // or delayMicroseconds(100); 

    // Test message to Serial1 to confirm it's working for USBHost_t36 logs
    // This message will go to pins 0/1
#ifdef USBHOST_PRINT_DEBUG  // Only try to print if USBHost_t36 debugging is enabled
    if (USBHDBGSerial) { // USBHDBGSerial is an alias for Serial1 (or whatever it's set to)
        USBHDBGSerial.println(F("--- SUH: USBHost_t36 logging (Serial1) initialized from startup_usbhost.cpp ---"));
        // USBHDBGSerial.flush(); // Ensure it's sent out
    }
#endif

    if (startup_populate_static_spoof_data() != 0) {
        printf("SUH: ERROR - Failed to populate static spoof data.\n"); // To Serial4
    }

    // This printf goes to Serial4
    printf("SUH: Attempting myusb.begin() for USB Host 2 init and power on...\n");
    
    // Now, USBHost_t36's print_() and println_() should output to Serial1 (pins 0/1)
    myusb.begin(); 
    
    // These printfs go to Serial4
    printf("SUH: myusb.begin() call completed.\n");
    printf("    SUH: USB2_PORTSC1 (Port Status) after myusb.begin(): 0x%08lX\n", USB2_PORTSC1);
    if (USB2_PORTSC1 & USB_PORTSC1_PP) { printf("    SUH: Port Power (PP) is ON for USB Host 2.\n"); } else { printf("    SUH: WARN - Port Power (PP) is OFF for USB Host 2!\n");}
    printf("    SUH: USB2_USBCMD (Command) after myusb.begin(): 0x%08lX\n", USB2_USBCMD);
    if (USB2_USBCMD & USB_USBCMD_RS) { printf("    SUH: USB Host 2 controller is RUNNING.\n");} else { printf("    SUH: USB Host 2 controller is STOPPED.\n");}
    
    printf("<<< SUH: startup_host_init_and_spoof finished.\n");
    return 0;
}
// --- END OF FILE startup_usbhost.cpp ---