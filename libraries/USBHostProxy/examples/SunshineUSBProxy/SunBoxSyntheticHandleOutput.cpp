#include "SunBoxSyntheticHandleOutput.h"
#include "SunBoxCommands.h"
#include "SunBoxUSBMouseDataHandler.h"

SunBoxSyntheticHandleOutput::SunBoxSyntheticHandleOutput(SunBoxCommands& commands, 
                                                       SunBoxUSBMouseDataHandler& usbHandler)
    : commands(commands), usbHandler(usbHandler), usbDeviceProxy(nullptr),
      mouseEndpoint(0) {
    previousUsbState.clear();
}

void SunBoxSyntheticHandleOutput::begin() {
    // Initialize if needed
}

void SunBoxSyntheticHandleOutput::process() {
    // Check if we have any data to process
    bool hasUSBData = usbHandler.hasData();
    bool hasSerialData = commands.hasData();
    
    // Early exit if no data
    if (!hasUSBData && !hasSerialData) {
        return;
    }
    
    // Check if USB device proxy is ready
    if (!usbDeviceProxy || !usbDeviceProxy->isConfigured() || mouseEndpoint == 0) {
        // Reset flags to prevent data buildup
        if (hasUSBData) usbHandler.reset();
        if (hasSerialData) commands.resetData();
        return;
    }
    
    // Check if endpoint is ready
    if (!usbDeviceProxy->isEndpointReady(mouseEndpoint)) {
        // Don't reset - let data accumulate for next cycle
        return;
    }
    
    // Get mouse states
    MouseState usbState;
    MouseState serialState;
    MouseState finalState;
    
    // Get USB data if available
    if (hasUSBData) {
        const uint8_t* rawData = usbHandler.getRawData();
        uint32_t rawLength = usbHandler.getRawDataLength();
        
        // Parse USB data
        usbHandler.getHIDHandler().parseMouseData(rawData, rawLength, usbState);
        usbHandler.reset();
    } else {
        // Use previous state for buttons (movement doesn't persist)
        usbState.buttons = previousUsbState.buttons;
        usbState.x = 0;
        usbState.y = 0;
        usbState.wheel = 0;
    }
    
    // Get serial data if available
    if (hasSerialData) {
        serialState = commands.getMouseState();
        commands.resetData();
    } else {
        serialState.clear();
    }
    
    // Combine the data - always add movements and OR buttons
    finalState.buttons = usbState.buttons | serialState.buttons;
    finalState.x = usbState.x + serialState.x;
    finalState.y = usbState.y + serialState.y;
    finalState.wheel = usbState.wheel + serialState.wheel;
    
    // Convert to raw format and send
    uint8_t outputBuffer[64];
    uint32_t outputLength = sizeof(outputBuffer);
    
    // Convert from standard format to device format
    usbHandler.getHIDHandler().formatMouseData(finalState, outputBuffer, outputLength);
    
    // Send the formatted data
    outputMouseData(outputBuffer, outputLength);
    
    // Update previous state
    if (hasUSBData) {
        previousUsbState = usbState;
    }
}

void SunBoxSyntheticHandleOutput::outputMouseData(const uint8_t* data, uint32_t length) {
    if (usbDeviceProxy && mouseEndpoint > 0) {
        usbDeviceProxy->sendDataOnEndpoint(mouseEndpoint, data, length);
    }
}