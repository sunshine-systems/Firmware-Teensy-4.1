#include "SunBoxEEPROM.h"
#include "SunBoxLogger.h"
#include "SunBoxStartup.h"

// Global instance
SunBoxEEPROM sunboxEEPROM;

SunBoxEEPROM::SunBoxEEPROM() : isInitialized(false) {
}

void SunBoxEEPROM::begin() {
    // Initialize EEPROM if needed
    isInitialized = true;
    
    logger.startup("Reading EEPROM...");
    
    // Verify EEPROM size is adequate
    if (EEPROM.length() < (EEPROM_DEBUG_ADDR + sizeof(DebugConfig))) {
        logger.warning("EEPROM size may be insufficient");
    }
    
    // Read and display all EEPROM values if debug is enabled
    if (SunBoxStartup::isDebugEnabled()) {
        // Check debug configuration
        DebugConfig debugConfig;
        EEPROM.get(EEPROM_DEBUG_ADDR, debugConfig);
        if (debugConfig.magic == EEPROM_DEBUG_MAGIC) {
            logger.debugf("Debug config: %s", debugConfig.debugEnabled ? "ENABLED" : "DISABLED");
        } else {
            logger.debug("No valid debug configuration found");
        }
        
        // Check claim configuration
        ClaimConfig claimConfig;
        EEPROM.get(EEPROM_CLAIM_ADDR, claimConfig);
        if (claimConfig.magic == EEPROM_CLAIM_MAGIC) {
            logger.debug("Found claim configuration:");
            logger.debugf("  VID: 0x%04X", claimConfig.vid);
            logger.debugf("  PID: 0x%04X", claimConfig.pid);
            logger.debugf("  Interface: %d", claimConfig.interface_num);
            logger.debugf("  Endpoint: 0x%02X", claimConfig.endpoint_addr);
            logger.debugf("  Endpoint Size: %d", claimConfig.endpoint_size);
        } else {
            logger.debug("No claim configuration found");
        }
    }
    
    logger.startup("Finished reading EEPROM");
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