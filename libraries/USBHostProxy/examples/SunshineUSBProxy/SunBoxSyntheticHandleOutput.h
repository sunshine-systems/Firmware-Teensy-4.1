#ifndef _SUNBOX_SYNTHETIC_HANDLE_OUTPUT_H_
#define _SUNBOX_SYNTHETIC_HANDLE_OUTPUT_H_

#include <Arduino.h>
#include "HIDMouseDescriptorHandler.h"
#include "USBDeviceProxy.h"

// Forward declarations
class SunBoxCommands;
class SunBoxUSBMouseDataHandler;

class SunBoxSyntheticHandleOutput {
public:
    SunBoxSyntheticHandleOutput(SunBoxCommands& commands, 
                               SunBoxUSBMouseDataHandler& usbHandler);
    
    // Initialize
    void begin();
    
    // Set USB Device Proxy reference
    void setUSBDeviceProxy(USBDeviceProxy* proxy) { usbDeviceProxy = proxy; }
    
    // Set mouse endpoint for output
    void setMouseEndpoint(uint8_t ep) { mouseEndpoint = ep; }
    
    // Process data from both sources and output
    void process();
    
private:
    // References
    SunBoxCommands& commands;
    SunBoxUSBMouseDataHandler& usbHandler;
    USBDeviceProxy* usbDeviceProxy;
    
    // Configuration
    uint8_t mouseEndpoint;
    
    // State tracking
    MouseState previousUsbState;
    
    // Helper methods
    void outputMouseData(const uint8_t* data, uint32_t length);
};

#endif // _SUNBOX_SYNTHETIC_HANDLE_OUTPUT_H_