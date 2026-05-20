#ifndef _SUNBOX_COMMANDS_H_
#define _SUNBOX_COMMANDS_H_

#include <Arduino.h>
#include "HIDMouseDescriptorHandler.h"

// Forward declarations
class CommandsSunBoxDevtoolsInterface;
class CommandsSunBoxInterface;

class SunBoxCommands {
public:
    SunBoxCommands(Stream& serial);
    ~SunBoxCommands();
    
    // Initialize all interfaces
    void begin();
    
    // Check for and process incoming commands/data
    void check();
    
    // Check if any interface has mouse data
    bool hasData() const;
    
    // Get the current mouse state
    MouseState getMouseState() const;
    
    // Reset data flags
    void resetData();
    
    // Get interface references (for external configuration)
    CommandsSunBoxDevtoolsInterface* getDevtools() { return devtoolsInterface; }
    CommandsSunBoxInterface* getLegacyInterface() { return legacyInterface; }
    
private:
    // Serial port
    Stream& serial;
    
    // Interface handlers
    CommandsSunBoxDevtoolsInterface* devtoolsInterface;
    CommandsSunBoxInterface* legacyInterface;
    
    // Routing buffer
    static const size_t BUFFER_SIZE = 256;
    uint8_t routingBuffer[BUFFER_SIZE];
    size_t bufferIndex;
    unsigned long lastCharTime;
    
    // Routing state
    enum RoutingMode {
        MODE_DETECTING,      // Analyzing what type of data this is
        MODE_DEVTOOLS,       // Text command for devtools
        MODE_LEGACY          // Binary legacy protocol
    };
    RoutingMode currentMode;

    // Helper methods
    void processBuffer();
    void detectAndRoute();
    void routeToDevtools();
    void routeToLegacy();
    void clearBuffer();
    bool isTextCommand(const uint8_t* data, size_t len);
};

#endif // _SUNBOX_COMMANDS_H_