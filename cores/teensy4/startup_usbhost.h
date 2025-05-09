// --- START OF FILE startup_usbhost.h ---
#ifndef STARTUP_USBHOST_H_
#define STARTUP_USBHOST_H_

#include <stdint.h>

// Data structure to hold the spoofed information
typedef struct {
    // ... (all your members for usb_proxy_info_t)
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    #define MAX_SPOOF_STRING_LEN 63
    char manufacturerString[MAX_SPOOF_STRING_LEN + 1];
    char productString[MAX_SPOOF_STRING_LEN + 1];
    char serialNumberString[MAX_SPOOF_STRING_LEN + 1];
    int      valid;
} usb_proxy_info_t;

#ifdef __cplusplus
extern "C" {
#endif

// This is the global variable defined in startup_usbhost.cpp
extern usb_proxy_info_t g_proxy_info;

// This is the function defined in startup_usbhost.cpp that we want to call from C
int startup_host_init_and_spoof(void); // <<< Declaration for C++ and C

// Original spoof data populator (if needed by C code elsewhere)
int startup_populate_static_spoof_data(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // STARTUP_USBHOST_H_
// --- END OF FILE startup_usbhost.h ---