#ifndef COMMANDS_SUNBOX_DEVTOOLS_INTERFACE_H
#define COMMANDS_SUNBOX_DEVTOOLS_INTERFACE_H

#include <Arduino.h>

// Forward declarations
class USBHostDriver;
class HIDMouseDescriptorHandler;

class CommandsSunBoxDevtoolsInterface {
public:
    CommandsSunBoxDevtoolsInterface();
    
    void begin();
    void handleCommand(const String& cmd);
    
    // Set component references
    void setUSBHostDriver(USBHostDriver* driver) { usbHostDriver = driver; }
    void setHIDHandler(HIDMouseDescriptorHandler* handler) { hidHandler = handler; }
    
    // Get current debug state
    bool isDebugEnabled() const { return debugEnabled; }
    
private:
    // Component references
    USBHostDriver* usbHostDriver;
    HIDMouseDescriptorHandler* hidHandler;
    
    // State
    bool debugEnabled;
    
    // Command handlers
    void handleHelp();
    void handleStatus();
    void handleDebug();
    void handleDump();
    void handleClaimCorrection(const String& args);
    void handleClaimClear();
    void handlePwrClear();  // Hidden command
    void handleDeltaLog();
};

#endif // COMMANDS_SUNBOX_DEVTOOLS_INTERFACE_H