#include "SunBoxAuth.h"
#include "SunBoxEEPROM.h"
#include "SunBoxLogger.h"
#include "imxrt.h"
#include <EEPROM.h>

// Static member initialization
bool SunBoxAuth::initialized = false;
bool SunBoxAuth::authorized = false;
uint64_t SunBoxAuth::hardwareId = 0;

// Obfuscation constants
#define AUTH_XOR_PATTERN 0xDEADC0DE13371337ULL
#define AUTH_SECRET_KEY  0xCAFEBABE87654321ULL
#define AUTH_MAGIC       0x53554E42  // 'SUNB'

void SunBoxAuth::begin() {
    if (initialized) {
        return;
    }
    
    initialized = true;
    
    // Initialize Serial4 for communication (always needed)
    Serial4.begin(115200);
    Serial4.setTimeout(0);  // Make serial operations non-blocking to prevent stalling when FT232H buffer is full
    delay(100);
    
    // Initialize logger with Serial4
    logger.begin(&Serial4);
    
    // Calculate hardware ID
    hardwareId = calculateHardwareId();
    
    // Check authorization status from EEPROM
    AuthConfig config;
    EEPROM.get(EEPROM_AUTH_ADDR, config);
    
    if (config.magic == AUTH_MAGIC) {
        // Check if stored device ID matches current hardware
        if (config.deviceId != hardwareId) {
            // Wrong device - this auth is for different hardware
            authorized = false;
        } else {
            // Calculate what the correct key should be for this hardware
            uint64_t expectedKey = hardwareId ^ AUTH_SECRET_KEY;
            
            // Reconstruct the stored key from the two 32-bit parts
            uint64_t storedKey = ((uint64_t)config.checksum << 32) | config.authKey;
            
            // Check if stored key matches the expected key
            authorized = (storedKey == expectedKey);
        }
        
        if (!authorized) {
            // Invalid authorization data - silent, no hints
        }
    } else {
        // No authorization found
        authorized = false;
    }
    
    // Only show startup message if authorized
    if (authorized) {
        LOG_STARTUP(LOG_BOOT, "SunBox Authorized");
    }
    // Silent if not authorized - no hints
}

bool SunBoxAuth::isAuthorized() {
    return authorized;
}

uint64_t SunBoxAuth::calculateHardwareId() {
    // Read unique ID from OCOTP registers
    uint32_t mac0 = HW_OCOTP_MAC0;
    uint32_t mac1 = HW_OCOTP_MAC1;
    
    // Combine into 64-bit value
    uint64_t id = ((uint64_t)mac1 << 32) | mac0;
    
    // XOR with pattern
    id ^= AUTH_XOR_PATTERN;
    
    // Rotate left by lower nibble of MAC0
    int rotateAmount = mac0 & 0x0F;
    id = rotateLeft(id, rotateAmount);
    
    return id;
}

uint64_t SunBoxAuth::rotateLeft(uint64_t value, int bits) {
    bits &= 63;  // Ensure bits is 0-63
    if (bits == 0) return value;
    return (value << bits) | (value >> (64 - bits));
}

uint64_t SunBoxAuth::getHardwareId() {
    if (!initialized) {
        hardwareId = calculateHardwareId();
    }
    return hardwareId;
}

bool SunBoxAuth::validateAuth(uint64_t hwId, uint64_t key, uint32_t checksum) {
    // Check if hardware ID matches
    if (hwId != hardwareId) {
        return false;
    }
    
    // Calculate expected key
    uint64_t expectedKey = hwId ^ AUTH_SECRET_KEY;
    
    // Calculate checksum of the key
    uint32_t calcChecksum = 0;
    uint64_t temp = expectedKey;
    for (int i = 0; i < 8; i++) {
        calcChecksum += (temp & 0xFF);
        temp >>= 8;
    }
    
    // Verify key matches (using first 32 bits)
    uint32_t keyPart = (uint32_t)(expectedKey & 0xFFFFFFFF);
    
    return (key == keyPart && checksum == calcChecksum);
}

bool SunBoxAuth::activate(uint64_t key, uint32_t checksum) {
    // Validate the provided key
    if (!validateAuth(hardwareId, key, checksum)) {
        return false;
    }
    
    // Store authorization in EEPROM
    AuthConfig config;
    config.magic = AUTH_MAGIC;
    config.deviceId = hardwareId;
    config.authKey = key;
    config.checksum = checksum;
    
    EEPROM.put(EEPROM_AUTH_ADDR, config);
    
    // Update authorized status
    authorized = true;
    
    return true;
}

void SunBoxAuth::deactivate() {
    // Clear authorization from EEPROM
    AuthConfig config;
    config.magic = 0;  // Invalid magic
    config.deviceId = 0;
    config.authKey = 0;
    config.checksum = 0;
    
    EEPROM.put(EEPROM_AUTH_ADDR, config);
    
    // Update authorized status
    authorized = false;
}

void SunBoxAuth::processCommand(const String& cmd) {
    String command = cmd;
    command.trim();
    
    if (command.length() == 0) {
        return;
    }
    
    // Process hidden commands (power-themed)
    if (command == "pwrdiag") {
        handleSunbox();  // Returns hardware ID
    }
    else if (command.startsWith("pwroverride ")) {
        String keyStr = command.substring(12);  // "pwroverride " is 12 chars
        handleMoonrise(keyStr);  // Set authorization
    }
    else if (command == "pwrreset") {
        handleEclipse();  // Clear authorization
    }
    // Silent - no error messages for unknown commands
}

void SunBoxAuth::handleSunbox() {
    // Get hardware ID
    uint64_t hwId = getHardwareId();
    
    // Output as 16-char hex string
    char buffer[17];
    sprintf(buffer, "%016llX", hwId);
    Serial4.println(buffer);
}

void SunBoxAuth::handleMoonrise(const String& keyStr) {
    // Parse hex string (should be 16 characters)
    String key = keyStr;
    key.trim();
    
    if (key.length() != 16) {
        // Silent failure - no error message
        return;
    }
    
    // Parse the hex string
    uint64_t authValue = 0;
    for (int i = 0; i < 16; i++) {
        char c = key.charAt(i);
        uint8_t nibble;
        
        if (c >= '0' && c <= '9') {
            nibble = c - '0';
        } else if (c >= 'A' && c <= 'F') {
            nibble = c - 'A' + 10;
        } else if (c >= 'a' && c <= 'f') {
            nibble = c - 'a' + 10;
        } else {
            // Invalid character - silent failure
            return;
        }
        
        authValue = (authValue << 4) | nibble;
    }
    
    // Store whatever was provided without validation
    // The validation will happen on next boot
    AuthConfig config;
    config.magic = AUTH_MAGIC;
    config.deviceId = hardwareId;  // Store current hardware ID
    config.authKey = (uint32_t)(authValue & 0xFFFFFFFF);  // Store lower 32 bits of input
    config.checksum = (uint32_t)(authValue >> 32);  // Store upper 32 bits as checksum
    
    EEPROM.put(EEPROM_AUTH_ADDR, config);
    
    // Always show success message (even if key is wrong)
    Serial4.println("Power cycle required.");
}

void SunBoxAuth::handleEclipse() {
    deactivate();
    Serial4.println("Power cycle required.");
}

// C-callable wrapper functions
extern "C" {
    void SunBoxStartup_authorize(void) {
        SunBoxAuth::begin();
    }
    
    uint8_t SunBoxStartup_isAuthorized(void) {
        return SunBoxAuth::isAuthorized() ? 1 : 0;
    }
}