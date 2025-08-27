#include "SunBoxUSBMouseDataHandler.h"
#include "SunBoxStartup.h"
#include "SunBoxLogger.h"

// Static instance for callback
SunBoxUSBMouseDataHandler* SunBoxUSBMouseDataHandler::instance = nullptr;

SunBoxUSBMouseDataHandler::SunBoxUSBMouseDataHandler(USBHostDriver& hostDriver, 
                                                   HIDMouseDescriptorHandler& hidHandler)
    : hostDriver(hostDriver), hidHandler(hidHandler),
      rawDataLength(0), dataAvailable(false),
      deviceReady(false), hidReady(false),
      buttonStateChanged(false), pendingButtonState(0) {
    
    memset(rawData, 0, sizeof(rawData));
    lastMouseState.clear();
    instance = this;
}

void SunBoxUSBMouseDataHandler::begin() {
    // Initialize HID handler
    hidHandler.begin(&hostDriver);
}

void SunBoxUSBMouseDataHandler::check() {
    // Check if device is ready
    if (!deviceReady && hostDriver.isReady()) {
        deviceReady = true;
        Serial4.println("S: Device detected");
        
        // Setup HID interface
        if (hidHandler.setupMouseInterface()) {
            Serial4.println("S: Mouse interface found");
            
            // Request HID descriptor
            if (hidHandler.requestHIDDescriptor(1000)) {
                if (hidHandler.isReady()) {
                    hidReady = true;
                    Serial4.println("S: HID ready");
                } else {
                    // Use boot protocol
                    hidHandler.setBootMouseFormat();
                    hidReady = true;
                    Serial4.println("S: Using boot protocol");
                }
                
                // Activate interface
                hidHandler.activateInterface();
            }
        }
    }
    
    // Check if device disconnected
    if (deviceReady && !hostDriver.isReady()) {
        deviceReady = false;
        hidReady = false;
        dataAvailable = false;
        rawDataLength = 0;
        Serial4.println("S: Device disconnected");
    }
}

void SunBoxUSBMouseDataHandler::dataCallback(const uint8_t* data, uint32_t length) {
    if (instance && data && length > 0) {
        // Just store raw data - no parsing in callback
        uint32_t copyLen = (length > sizeof(instance->rawData)) ? sizeof(instance->rawData) : length;
        memcpy(instance->rawData, data, copyLen);
        instance->rawDataLength = copyLen;
        instance->dataAvailable = true;
        
        // Parse and check for button changes
        if (instance->hidReady && instance->hidHandler.isReady()) {
            MouseState state;
            if (instance->hidHandler.parseMouseData(data, length, state)) {
                // Check if buttons changed
                if (state.buttons != instance->lastMouseState.buttons) {
                    // Store for deferred printing (safe from interrupts)
                    instance->pendingButtonState = state.buttons;
                    instance->buttonStateChanged = true;
                    instance->lastMouseState.buttons = state.buttons;
                }
            }
        }
    }
}

void SunBoxUSBMouseDataHandler::reset() {
    dataAvailable = false;
    // Keep raw data and length for debugging if needed
}

bool SunBoxUSBMouseDataHandler::isReady() const {
    return deviceReady && hidReady;
}

void SunBoxUSBMouseDataHandler::processPendingButtonChanges() {
    // Check if there's a pending button state change to print
    if (buttonStateChanged) {
        // Capture the state and clear the flag atomically
        uint8_t buttonsToPrint = pendingButtonState;
        buttonStateChanged = false;
        
        // Now safely print from main loop context (not interrupt)
        logger.mousef("%02X", buttonsToPrint);
    }
}