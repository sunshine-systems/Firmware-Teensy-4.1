// SunBoxStartup.cpp
#include "SunBoxStartup.h"
#include "Arduino.h"

// Static member initialization
bool SunBoxStartup::initialized = false;
bool SunBoxStartup::ready = false;

void SunBoxStartup::begin() {
    if (initialized) {
        return;
    }
    
    initialized = true;
    
    // Just initialize Serial4 here - that's all we need early on
    Serial4.begin(115200);
    delay(100);
    Serial4.println("[STARTUP]: SunBox early initialization complete.");
    
    // Don't create any USB objects here - let the main sketch do it
    ready = true;
}

bool SunBoxStartup::isReady() {
    return ready;
}

// C-callable wrapper functions
extern "C" {
    void SunBoxStartup_begin(void) {
        SunBoxStartup::begin();
    }
    
    uint8_t SunBoxStartup_isReady(void) {
        return SunBoxStartup::isReady() ? 1 : 0;
    }
}