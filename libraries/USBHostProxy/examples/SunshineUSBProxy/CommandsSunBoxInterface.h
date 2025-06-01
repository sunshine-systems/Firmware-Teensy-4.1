#ifndef _COMMANDS_SUNBOX_INTERFACE_H_
#define _COMMANDS_SUNBOX_INTERFACE_H_

#include <Arduino.h>
#include "HIDMouseDescriptorHandler.h"

// Firmware settings structure
struct FirmwareSettings {
    uint8_t sensitivity;
    uint8_t acceleration;
    uint8_t smoothing;
    
    void updateSettings(const uint8_t* data);
};

class CommandsSunBoxInterface {
public:
    CommandsSunBoxInterface();
    
    void begin();
    
    // Process serial data for legacy protocol
    void processSerial(Stream& serial);
    
    // Direct data processing (for routing)
    void processLegacyData(const uint8_t* data, uint8_t length);
    
    // Check if data is available
    bool hasData() const { return isDataAvailable; }
    
    // Reset data flag (prevent ghosting)
    void reset();
    
    // Get mouse state
    MouseState getMouseState() const;
    
    // Get previous mouse buttons (for detecting changes)
    uint8_t getPreviousMouseButtons() const { return previousMouseButtons; }
    
    // Get firmware settings
    FirmwareSettings& getSettings() { return firmwareSettings; }
    
private:
    // Mouse state
    uint8_t previousMouseButtons;
    uint8_t mouseButtons;
    uint8_t scrollWheel;
    int16_t xMovement;
    int16_t yMovement;
    bool isDataAvailable;
    
    // Settings
    FirmwareSettings firmwareSettings;
    
    // Process HID report data
    void processAndSetHIDReportData(const uint8_t* data);
    
    // Parse functions (to be implemented based on actual protocol)
    int16_t parseX_00A6(const uint8_t* data);
    int16_t parseY_00A6(const uint8_t* data);
};

#endif // _COMMANDS_SUNBOX_INTERFACE_H_