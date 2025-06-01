#include "SunBoxEEPROM.h"

// Global instance
SunBoxEEPROM sunboxEEPROM;

SunBoxEEPROM::SunBoxEEPROM() : isInitialized(false) {
}

void SunBoxEEPROM::begin() {
    // Initialize EEPROM if needed
    isInitialized = true;
    
    // Verify EEPROM size is adequate
    if (EEPROM.length() < (EEPROM_DEBUG_ADDR + sizeof(DebugConfig))) {
        Serial4.println("[EEPROM]: Warning - EEPROM size may be insufficient");
    }
}

bool SunBoxEEPROM::saveClaimConfig(uint16_t vid, uint16_t pid, uint8_t interface_num, 
                                   uint8_t endpoint_addr, uint16_t endpoint_size) {
    if (!isInitialized) {
        begin();
    }
    
    ClaimConfig config;
    config.magic = EEPROM_CLAIM_MAGIC;
    config.vid = vid;
    config.pid = pid;
    config.interface_num = interface_num;
    config.endpoint_addr = endpoint_addr;
    config.endpoint_size = endpoint_size;
    
    EEPROM.put(EEPROM_CLAIM_ADDR, config);
    
    return true;
}

bool SunBoxEEPROM::loadClaimConfig(ClaimConfig& config) {
    if (!isInitialized) {
        begin();
    }
    
    EEPROM.get(EEPROM_CLAIM_ADDR, config);
    
    return (config.magic == EEPROM_CLAIM_MAGIC);
}

void SunBoxEEPROM::clearClaimConfig() {
    if (!isInitialized) {
        begin();
    }
    
    ClaimConfig config;
    config.magic = 0; // Invalid magic
    EEPROM.put(EEPROM_CLAIM_ADDR, config);
}

bool SunBoxEEPROM::hasValidClaimConfig() {
    ClaimConfig config;
    return loadClaimConfig(config);
}

bool SunBoxEEPROM::saveDebugMode(bool enabled) {
    if (!isInitialized) {
        begin();
    }
    
    DebugConfig config;
    config.magic = EEPROM_DEBUG_MAGIC;
    config.debugEnabled = enabled;
    
    EEPROM.put(EEPROM_DEBUG_ADDR, config);
    
    return true;
}

bool SunBoxEEPROM::loadDebugMode(bool& enabled) {
    if (!isInitialized) {
        begin();
    }
    
    DebugConfig config;
    EEPROM.get(EEPROM_DEBUG_ADDR, config);
    
    if (config.magic == EEPROM_DEBUG_MAGIC) {
        enabled = config.debugEnabled;
        return true;
    }
    
    // Default to false if not valid
    enabled = false;
    return false;
}

bool SunBoxEEPROM::toggleDebugMode() {
    bool currentState = false;
    loadDebugMode(currentState);
    
    bool newState = !currentState;
    saveDebugMode(newState);
    
    return newState;
}

void SunBoxEEPROM::clearAll() {
    if (!isInitialized) {
        begin();
    }
    
    clearClaimConfig();
    
    // Clear debug config
    DebugConfig debugConfig;
    debugConfig.magic = 0;
    EEPROM.put(EEPROM_DEBUG_ADDR, debugConfig);
}