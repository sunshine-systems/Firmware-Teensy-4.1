#include "usb_host_wrapper.h"
#include "USBHostDriver.h"

// Global pointer to USBHostDriver instance
USBHostDriver* g_usbHostDriver = nullptr;

extern "C" {

bool usb_host_is_ready(void) {
    if (g_usbHostDriver) {
        return g_usbHostDriver->isReady();
    }
    return false;
}

bool usb_host_control_transfer(uint8_t bmRequestType, uint8_t bRequest, 
                              uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                              uint8_t* data, uint16_t* actualLength, 
                              uint32_t timeout_ms) {
    if (g_usbHostDriver) {
        return g_usbHostDriver->controlTransfer(
            bmRequestType, bRequest, wValue, wIndex, wLength,
            data, actualLength, timeout_ms
        );
    }
    return false;
}

void usb_host_pause_transfers(void) {
    if (g_usbHostDriver) {
        g_usbHostDriver->pauseDataTransfers();
    }
}

void usb_host_resume_transfers(void) {
    if (g_usbHostDriver) {
        g_usbHostDriver->resumeDataTransfers();
    }
}

uint16_t usb_host_get_vid(void) {
    if (g_usbHostDriver) {
        return g_usbHostDriver->getVendorID();
    }
    return 0;
}

uint16_t usb_host_get_pid(void) {
    if (g_usbHostDriver) {
        return g_usbHostDriver->getProductID();
    }
    return 0;
}

} // extern "C"