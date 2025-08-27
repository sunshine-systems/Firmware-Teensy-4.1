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
    
    // Check for device status changes only (no data processing)
    void check();
    
    // Check if data is available
    bool hasData() const { return dataAvailable; }
    
    // Get the raw data
    const uint8_t* getRawData() const { return rawData; }
    uint32_t getRawDataLength() const { return rawDataLength; }
    
    // Get HID handler for parsing
    HIDMouseDescriptorHandler& getHIDHandler() { return hidHandler; }
    
    // Reset data flag (prevents ghosting)
    void reset();
    
    // Check if USB device is ready
    bool isReady() const;
    
    // Static callback for USB data
    static void dataCallback(const uint8_t* data, uint32_t length);
    
    // Process any pending button state changes (call from main loop)
    void processPendingButtonChanges();
    
private:
    // References
    USBHostDriver& hostDriver;
    HIDMouseDescriptorHandler& hidHandler;
    
    // Raw data storage (no parsing in callback)
    uint8_t rawData[64];
    uint32_t rawDataLength;
    bool dataAvailable;
    
    // State
    bool deviceReady;
    bool hidReady;
    MouseState lastMouseState;  // Track last state for button change detection
    
    // Volatile variables for deferred button printing (safe from interrupts)
    volatile bool buttonStateChanged;
    volatile uint8_t pendingButtonState;
    
    // Static instance for callback
    static SunBoxUSBMouseDataHandler* instance;
};

#endif // _SUNBOX_USB_MOUSE_DATA_HANDLER_H_