#ifndef _SUNBOX_SYNTHETIC_HANDLE_OUTPUT_H_
#define _SUNBOX_SYNTHETIC_HANDLE_OUTPUT_H_

#include <Arduino.h>
#include "HIDMouseDescriptorHandler.h"

// Forward declarations
class SunBoxCommands;
class SunBoxUSBMouseDataHandler;

class SunBoxSyntheticHandleOutput {
public:
    SunBoxSyntheticHandleOutput(SunBoxCommands& commands, 
                               SunBoxUSBMouseDataHandler& usbHandler);
    
    // Initialize
    void begin();
    
    // Process data from both sources
    void process();
    
    // Configuration for data mixing
    enum MixMode {
        MIX_MODE_USB_ONLY,      // Only use USB data
        MIX_MODE_SERIAL_ONLY,   // Only use serial data
        MIX_MODE_BOTH_REPLACE,  // Use both, serial replaces USB
        MIX_MODE_BOTH_ADD       // Use both, add movements
    };
    
    void setMixMode(MixMode mode) { mixMode = mode; }
    MixMode getMixMode() const { return mixMode; }
    
private:
    // References
    SunBoxCommands& commands;
    SunBoxUSBMouseDataHandler& usbHandler;
    
    // Configuration
    MixMode mixMode;
    
    // Mixed output state
    MouseState mixedState;
    MouseState lastOutputState;  // Track last output for change detection
    
    // Helper methods
    void mixMouseData(const MouseState& usbState, const MouseState& serialState);
    void outputMouseData(const MouseState& state);
};

#endif // _SUNBOX_SYNTHETIC_HANDLE_OUTPUT_H_