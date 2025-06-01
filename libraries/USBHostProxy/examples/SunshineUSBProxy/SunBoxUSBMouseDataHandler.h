#ifndef _SUNBOX_USB_MOUSE_DATA_HANDLER_H_
#define _SUNBOX_USB_MOUSE_DATA_HANDLER_H_

#include <Arduino.h>
#include "USBHostDriver.h"
#include "HIDMouseDescriptorHandler.h"

class SunBoxUSBMouseDataHandler {
public:
    SunBoxUSBMouseDataHandler(USBHostDriver& hostDriver, HIDMouseDescriptorHandler& hidHandler);
    
    // Initialize the handler
    void begin();
    
    // Check for new data from USB device
    void check();
    
    // Check if data is available
    bool hasData() const { return dataAvailable; }
    
    // Get the current mouse state
    MouseState getMouseState() const { return currentMouseState; }
    
    // Get raw data
    const uint8_t* getRawData() const { return rawData; }
    uint32_t getRawDataLength() const { return rawDataLength; }
    
    // Reset data flag
    void resetData();
    
    // Check if USB device is ready
    bool isReady() const;
    
private:
    // References
    USBHostDriver& hostDriver;
    HIDMouseDescriptorHandler& hidHandler;
    
    // Data storage
    uint8_t rawData[64];
    uint32_t rawDataLength;
    MouseState currentMouseState;
    bool dataAvailable;
    
    // State
    bool deviceReady;
    bool hidReady;
    
    // Callback for USB data
    static void dataCallback(const uint8_t* data, uint32_t length);
    static SunBoxUSBMouseDataHandler* instance;
};

#endif // _SUNBOX_USB_MOUSE_DATA_HANDLER_H_