#ifndef _COMMANDS_SUNBOX_DEVTOOLS_INTERFACE_H_
#define _COMMANDS_SUNBOX_DEVTOOLS_INTERFACE_H_

#include <Arduino.h>

// Forward declarations
class USBHostDriver;
class HIDMouseDescriptorHandler;

class CommandsSunBoxDevtoolsInterface {
public:
    CommandsSunBoxDevtoolsInterface();
    
    void begin();
    
    // Handle a command
    void handleCommand(const String& command);
    
    // Set references to components (for dump/status commands)
    void setUSBHostDriver(USBHostDriver* driver) { usbHostDriver = driver; }
    void setHIDHandler(HIDMouseDescriptorHandler* handler) { hidHandler = handler; }
    
    // Debug mode
    bool isDebugEnabled() const { return debugEnabled; }
    
private:
    // Component references
    USBHostDriver* usbHostDriver;
    HIDMouseDescriptorHandler* hidHandler;
    
    // Settings
    bool debugEnabled;
    
    // Command handlers
    void handleHelp();
    void handleStatus();
    void handleDebug();
    void handleDump();
    void handleClaimCorrection(const String& args);
    void handleClaimClear();
};

#endif // _COMMANDS_SUNBOX_DEVTOOLS_INTERFACE_H_