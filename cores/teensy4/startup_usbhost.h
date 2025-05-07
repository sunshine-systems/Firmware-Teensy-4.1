#ifndef STARTUP_USBHOST_H_
#define STARTUP_USBHOST_H_

#include <stdint.h>

// Data structure to hold the spoofed information
typedef struct {
    // --- Core Device Info to Spoof ---
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  bDeviceClass;    // If 0, class is per-interface
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0; // Usually 64

    // --- String Descriptors to Spoof (as C strings) ---
    #define MAX_SPOOF_STRING_LEN 63 // Max 63 chars + null
    char manufacturerString[MAX_SPOOF_STRING_LEN + 1];
    char productString[MAX_SPOOF_STRING_LEN + 1];
    char serialNumberString[MAX_SPOOF_STRING_LEN + 1];

    // --- HID Report Descriptor (OPTIONAL for this simplified test) ---
    // If you want to also spoof this for a simple HID device (like RawHID's own report descriptor)
    // For now, we'll leave it out to keep it simple. If spoofed, usb_dev.c would need to handle it.
    // const uint8_t* hid_report_descriptor;
    // uint16_t hid_report_descriptor_len;
    // uint8_t  hid_interface_number_for_report;

    int      valid; // 1 if this static data should be used
} usb_proxy_info_t;

#ifdef __cplusplus
extern "C" {
#endif

extern usb_proxy_info_t g_proxy_info; // Defined in startup_usbhost.cpp

// This function will now just populate g_proxy_info with static data
int startup_populate_static_spoof_data(void);

#ifdef __cplusplus
}
#endif

#endif // STARTUP_USBHOST_H_