#include "SunBoxSyntheticHandleOutput.h"
#include "SunBoxCommands.h"
#include "SunBoxUSBMouseDataHandler.h"
#include "SunBoxStartup.h"

extern "C" {
    // TODO: Include your USB device mouse functions here
    // Example: void usb_mouse_move(int16_t x, int16_t y, int8_t wheel);
    // Example: void usb_mouse_buttons(uint8_t buttons);
}

SunBoxSyntheticHandleOutput::SunBoxSyntheticHandleOutput(SunBoxCommands& commands, 
                                                       SunBoxUSBMouseDataHandler& usbHandler)
    : commands(commands), usbHandler(usbHandler), mixMode(MIX_MODE_BOTH_ADD) {
    mixedState.clear();
    lastOutputState.clear();
}

void SunBoxSyntheticHandleOutput::begin() {
    // Initialize if needed
}

void SunBoxSyntheticHandleOutput::process() {
    MouseState usbState;
    MouseState serialState;
    bool hasUSBData = false;
    bool hasSerialData = false;
    
    // Get USB data if available
    if (usbHandler.hasData()) {
        usbState = usbHandler.getMouseState();
        usbHandler.resetData();
        hasUSBData = true;
    } else {
        usbState.clear();
    }
    
    // Get serial data if available
    if (commands.hasData()) {
        serialState = commands.getMouseState();
        commands.resetData();
        hasSerialData = true;
    } else {
        serialState.clear();
    }
    
    // Mix based on mode
    switch (mixMode) {
        case MIX_MODE_USB_ONLY:
            if (hasUSBData) {
                outputMouseData(usbState);
            }
            break;
            
        case MIX_MODE_SERIAL_ONLY:
            if (hasSerialData) {
                outputMouseData(serialState);
            }
            break;
            
        case MIX_MODE_BOTH_REPLACE:
            if (hasSerialData) {
                outputMouseData(serialState);
            } else if (hasUSBData) {
                outputMouseData(usbState);
            }
            break;
            
        case MIX_MODE_BOTH_ADD:
            if (hasUSBData || hasSerialData) {
                mixMouseData(usbState, serialState);
                outputMouseData(mixedState);
            }
            break;
    }
}

void SunBoxSyntheticHandleOutput::mixMouseData(const MouseState& usbState, const MouseState& serialState) {
    // Add movements
    mixedState.x = usbState.x + serialState.x;
    mixedState.y = usbState.y + serialState.y;
    mixedState.wheel = usbState.wheel + serialState.wheel;
    
    // OR buttons
    mixedState.buttons = usbState.buttons | serialState.buttons;
}

void SunBoxSyntheticHandleOutput::outputMouseData(const MouseState& state) {
    bool debug_enabled = SunBoxStartup::isDebugEnabled();
    
    // Always output debug info when buttons change or there's movement
    static MouseState lastOutputState;
    static unsigned long lastDebugTime = 0;
    
    // Check if anything changed since last output
    bool stateChanged = (state.buttons != lastOutputState.buttons ||
                        state.x != lastOutputState.x ||
                        state.y != lastOutputState.y ||
                        state.wheel != lastOutputState.wheel);
    
    if (stateChanged && debug_enabled) {
        // Rate limit debug output to prevent flooding
        if (millis() - lastDebugTime > 50) {  // 50ms minimum between outputs
            Serial4.print("I: Mouse - Buttons:0x");
            if (state.buttons < 0x10) Serial4.print("0");
            Serial4.print(state.buttons, HEX);
            Serial4.print(" X:");
            Serial4.print(state.x);
            Serial4.print(" Y:");
            Serial4.print(state.y);
            Serial4.print(" Wheel:");
            Serial4.println(state.wheel);
            lastDebugTime = millis();
        }
        
        // Update last output state
        lastOutputState = state;
    }
    
    // TODO: Send to USB device stack
    // Example:
    // usb_mouse_move(state.x, state.y, state.wheel);
    // usb_mouse_buttons(state.buttons);
}