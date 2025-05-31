// USBHostProxy.cpp
#include "USBHostProxy.h"
#include "Arduino.h"  // For millis(), Serial, etc.

// Include your C++ libraries here
// #include "SomeLibrary.h"

// Global flag (C linkage)
volatile uint8_t readyForProxy = 0;

// Global C++ instance
USBHostProxy usbHostProxy;

// C++ Class Implementation
USBHostProxy::USBHostProxy() : startTime(0), sequenceStarted(false), ready(false) {
}

void USBHostProxy::begin() {
    if (!sequenceStarted) {
        Serial.println("USBHostProxy: Starting proxy sequence...");
        startTime = millis();
        sequenceStarted = true;
        ready = false;
        readyForProxy = 0;
        
        // Initialize your C++ libraries here
        // myLib.begin();
    }
}

void USBHostProxy::update() {
    if (sequenceStarted && !ready) {
        uint32_t elapsed = millis() - startTime;
        
        // Print status every second
        static uint32_t lastPrint = 0;
        if (elapsed / 1000 > lastPrint) {
            lastPrint = elapsed / 1000;
            Serial.print("USBHostProxy: Waiting... ");
            Serial.print(lastPrint);
            Serial.println(" seconds");
        }
        
        // After 4 seconds, mark as ready
        if (elapsed >= 4000) {
            Serial.println("USBHostProxy: Proxy initialization complete!");
            ready = true;
            readyForProxy = 1;
            
            // Add your actual USB host detection logic here
            // Example: myLib.detectDevice();
        }
    }
}

bool USBHostProxy::isReady() const {
    return ready;
}

// C-callable wrapper functions (extern "C" linkage)
extern "C" {
    void USBHostProxy_startSequence(void) {
        if (!usbHostProxy.isReady()) {
            usbHostProxy.begin();
            usbHostProxy.update();
        }
    }
    
    uint8_t USBHostProxy_isReady(void) {
        return readyForProxy;
    }
}