// SunBoxStartup.cpp
#include "SunBoxStartup.h"
#include "Arduino.h"
#include <EEPROM.h>
#include "SunBoxEEPROM.h"  // For the struct definitions and constants
#include "SunBoxLogger.h"  // For logger
#include "SunBoxAuth.h"    // For authorization

// Static member initialization
bool SunBoxStartup::initialized = false;
bool SunBoxStartup::ready = false;
bool SunBoxStartup::debugEnabled = false;

void SunBoxStartup::begin() {
    if (initialized) {
        return;
    }
    
    initialized = true;
    
    // Note: Serial4 is already initialized by SunBoxAuth in SunBoxStartup_authorize()
    // Logger is also already initialized
    
    // Only proceed if authorized
    if (!SunBoxAuth::isAuthorized()) {
        // Silent failure - don't set ready flag
        return;
    }
    
    LOG_STARTUP(LOG_BOOT, "SunBox Initializing...");
    
    // Load debug mode from EEPROM directly (can't use sunboxEEPROM object yet)
    DebugConfig config;
    EEPROM.get(EEPROM_DEBUG_ADDR, config);
    
    if (config.magic == EEPROM_DEBUG_MAGIC) {
        debugEnabled = config.debugEnabled;
    } else {
        debugEnabled = false;
    }
    
    LOG_STARTUP(LOG_BOOT, "SunBox Initialized...");
    
    // Don't create any USB objects here - let the main sketch do it
    ready = true;
}

bool SunBoxStartup::isReady() {
    return ready;
}

bool SunBoxStartup::isDebugEnabled() {
    return debugEnabled;
}

void SunBoxStartup::setDebugEnabled(bool enabled) {
    debugEnabled = enabled;
}

// C-callable wrapper functions - MUST be in extern "C" block
extern "C" {
    void SunBoxStartup_begin(void) {
        SunBoxStartup::begin();
    }
    
    uint8_t SunBoxStartup_isReady(void) {
        return SunBoxStartup::isReady() ? 1 : 0;
    }
    
    uint8_t SunBoxStartup_isDebugEnabled(void) {
        return SunBoxStartup::isDebugEnabled() ? 1 : 0;
    }
}