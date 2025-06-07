#include "SunBoxUSBMouseDataHandler.h"
#include "SunBoxStartup.h"

// Static instance for callback
SunBoxUSBMouseDataHandler* SunBoxUSBMouseDataHandler::instance = nullptr;

SunBoxUSBMouseDataHandler::SunBoxUSBMouseDataHandler(USBHostDriver& hostDriver, 
                                                   HIDMouseDescriptorHandler& hidHandler)
    : hostDriver(hostDriver), hidHandler(hidHandler),
      rawDataLength(0), dataAvailable(false),
      deviceReady(false), hidReady(false) {
    
    memset(rawData, 0, sizeof(rawData));
    currentMouseState.clear();
    instance = this;
}

void SunBoxUSBMouseDataHandler::begin() {
    // Don't set up callback here - let the main sketch do it
    // This allows the main sketch to use its own callback for data forwarding
    
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
        currentMouseState.clear();
        Serial4.println("S: Device disconnected");
    }
}

void SunBoxUSBMouseDataHandler::dataCallback(const uint8_t* data, uint32_t length) {
    if (instance && data && length > 0) {
        // Store raw data
        uint32_t copyLen = (length > sizeof(instance->rawData)) ? sizeof(instance->rawData) : length;
        memcpy(instance->rawData, data, copyLen);
        instance->rawDataLength = copyLen;
        
        // Parse if HID is ready
        if (instance->hidReady && instance->hidHandler.isReady()) {
            // Store previous state to detect changes
            MouseState previousState = instance->currentMouseState;
            
            // Parse new data
            if (instance->hidHandler.parseMouseData(data, length, instance->currentMouseState)) {
                // Check if anything changed (buttons, movement, or wheel)
                if (previousState.buttons != instance->currentMouseState.buttons ||
                    previousState.x != instance->currentMouseState.x ||
                    previousState.y != instance->currentMouseState.y ||
                    previousState.wheel != instance->currentMouseState.wheel) {
                    
                    // Mark data as available only if something changed
                    instance->dataAvailable = true;
                    
                    // Print the parsed state for debugging
                    if (SunBoxStartup::isDebugEnabled()) {
                        instance->hidHandler.printMouseState(instance->currentMouseState);
                    }
                }
            }
        } else {
            // Just mark raw data as available if HID not ready
            instance->dataAvailable = true;
        }
    }
}

void SunBoxUSBMouseDataHandler::resetData() {
    dataAvailable = false;
    // Don't clear the mouse state here - keep the last known state
    // This prevents "jumps" when transitioning between data sources
}

bool SunBoxUSBMouseDataHandler::isReady() const {
    return deviceReady && hidReady;
}