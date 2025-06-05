#ifndef _USB_HOST_WRAPPER_H_
#define _USB_HOST_WRAPPER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

// C-compatible interface to USBHostDriver
bool usb_host_is_ready(void);
bool usb_host_control_transfer(uint8_t bmRequestType, uint8_t bRequest, 
                              uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                              uint8_t* data, uint16_t* actualLength, 
                              uint32_t timeout_ms);
void usb_host_pause_transfers(void);
void usb_host_resume_transfers(void);
uint16_t usb_host_get_vid(void);
uint16_t usb_host_get_pid(void);

#ifdef __cplusplus
}
#endif

#endif // _USB_HOST_WRAPPER_H_