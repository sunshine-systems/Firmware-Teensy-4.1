#ifndef _SUNBOX_SYNTHETIC_HANDLE_OUTPUT_H_
#define _SUNBOX_SYNTHETIC_HANDLE_OUTPUT_H_

#include <Arduino.h>
#include "HIDMouseDescriptorHandler.h"
#include "USBDeviceProxy.h"

// Mouse button definitions (matching Arduino)
#define MOUSE_LEFT      1
#define MOUSE_RIGHT     2
#define MOUSE_MIDDLE    4
#define MOUSE_BUTTON4   8
#define MOUSE_BUTTON5   0x10

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
    
    // State tracking for USB
    MouseState previousUsbState;
    uint8_t previousUsbButtons;
    
    // State tracking for Serial
    uint8_t previousSerialButtons;
    
    // Timestamps for button events
    unsigned long activationTimestamp4MouseButtonExclusion;
    unsigned long activationTimestamp4MouseMovementLockout;
    unsigned long lastRMBPressTime;
    unsigned long lastLMBPressTime;
    unsigned long lastMB4PressTime;
    unsigned long lastMB5PressTime;
    
    // Sensitivity reduction accumulators
    int sensReductionXAccumulator;
    int sensReductionYAccumulator;
    
    // Spin bot state
    bool spinActive;
    int spinRotationsRemaining;
    unsigned long spinNextMoveTime;
    int spinCurrentX;
    
    // Helper methods
    void outputMouseData(const uint8_t* data, uint32_t length);
    void performButtonFiltering(uint8_t& buttons, uint8_t previousButtons, uint8_t unmodifiedButtons);
    void modifyMovementWithSerialData(int16_t& usbX, int16_t& usbY, int16_t serialX, int16_t serialY);
    void handleSpinBot(uint8_t currentButtons, uint8_t previousButtons, bool isSerialPress);
    void updateSpinBot(int16_t& xMovement, int16_t& yMovement);
    bool shouldExcludeButton(uint8_t currentButtons, uint8_t previousButtons, uint8_t buttonMask);
    void handleMouseButtonConfigCheck(uint8_t& buttons, uint8_t unmodifiedButtons, uint8_t previousButtons, 
                                     uint8_t buttonMask, int disablePassthroughOption, unsigned long& lastPressTime);
    void logMouseEvent(uint8_t buttons);
};

#endif // _SUNBOX_SYNTHETIC_HANDLE_OUTPUT_H_