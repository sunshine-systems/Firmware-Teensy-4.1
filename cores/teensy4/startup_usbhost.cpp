// --- START OF FILE startup_usbhost.cpp ---
#include "startup_usbhost.h"
#include "imxrt.h"
#include "USBHost_t36.h"
#include <string.h>
#include "debug/printf.h"

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
    printf(">>> SUH: startup_host_init_and_spoof called.\n");
    if (startup_populate_static_spoof_data() != 0) {
        printf("SUH: ERROR - Failed to populate static spoof data.\n");
    }
    printf("SUH: Attempting myusb.begin() for USB Host 2 init and power on...\n");
    myusb.begin();
    printf("SUH: myusb.begin() call completed.\n");
    printf("    SUH: USB2_PORTSC1 (Port Status) after myusb.begin(): 0x%08lX\n", USB2_PORTSC1);
    if (USB2_PORTSC1 & USB_PORTSC1_PP) { printf("    SUH: Port Power (PP) is ON for USB Host 2.\n"); } else { printf("    SUH: WARN - Port Power (PP) is OFF for USB Host 2!\n");}
    printf("    SUH: USB2_USBCMD (Command) after myusb.begin(): 0x%08lX\n", USB2_USBCMD);
    if (USB2_USBCMD & USB_USBCMD_RS) { printf("    SUH: USB Host 2 controller is RUNNING.\n");} else { printf("    SUH: USB Host 2 controller is STOPPED.\n");}
    printf("<<< SUH: startup_host_init_and_spoof finished.\n");
    return 0;
}
// --- END OF FILE startup_usbhost.cpp ---