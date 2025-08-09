#ifndef _SUNBOX_EEPROM_H_
#define _SUNBOX_EEPROM_H_

#include <Arduino.h>
#include <EEPROM.h>

// EEPROM magic numbers for validation
#define EEPROM_CLAIM_MAGIC 0xDEADBEEF
#define EEPROM_DEBUG_MAGIC 0xCAFEBABE

// EEPROM addresses
#define EEPROM_CLAIM_ADDR 0
#define EEPROM_DEBUG_ADDR (EEPROM_CLAIM_ADDR + sizeof(ClaimConfig))
#define EEPROM_AUTH_ADDR  0x20  // Fixed address to avoid overlap

// Structures
struct ClaimConfig {
    uint32_t magic;      // 0xDEADBEEF for valid data
    uint16_t vid;
    uint16_t pid;
    uint8_t interface_num;
    uint8_t endpoint_addr;
    uint16_t endpoint_size;
};

struct DebugConfig {
    uint32_t magic;      // 0xCAFEBABE for valid data
    bool debugEnabled;
};

struct AuthConfig {
    uint32_t magic;         // 0x53554E42 ('SUNB')
    uint64_t deviceId;      // Processed hardware ID
    uint32_t authKey;       // Authorization key (first 4 bytes)
    uint32_t checksum;      // Validation checksum
};

class SunBoxEEPROM {
public:
    SunBoxEEPROM();
    
    // Initialize EEPROM
    void begin();
    
    // Claim configuration
    bool saveClaimConfig(uint16_t vid, uint16_t pid, uint8_t interface_num, 
                        uint8_t endpoint_addr, uint16_t endpoint_size = 64);
    bool loadClaimConfig(ClaimConfig& config);
    void clearClaimConfig();
    bool hasValidClaimConfig();
    
    // Debug configuration
    bool saveDebugMode(bool enabled);
    bool loadDebugMode(bool& enabled);
    bool toggleDebugMode(); // Returns new state
    
    // Authorization configuration
    bool saveAuthConfig(const AuthConfig& config);
    bool loadAuthConfig(AuthConfig& config);
    void clearAuthConfig();
    bool hasValidAuthConfig();
    
    // Utility
    void clearAll();
    
private:
    // Helper to verify EEPROM is initialized
    bool isInitialized;
};

// Global instance
extern SunBoxEEPROM sunboxEEPROM;

#endif // _SUNBOX_EEPROM_H_