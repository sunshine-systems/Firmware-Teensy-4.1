#ifndef _SUNBOX_AUTH_H_
#define _SUNBOX_AUTH_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// C-callable functions for startup.c
void SunBoxStartup_authorize(void);
uint8_t SunBoxStartup_isAuthorized(void);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <Arduino.h>
#include "SunBoxEEPROM.h"  // For AuthConfig struct

class SunBoxAuth {
public:
    // Initialize authorization system and Serial4
    static void begin();
    
    // Check if device is authorized
    static bool isAuthorized();
    
    // Process activation commands (for unauthorized mode)
    static void processCommand(const String& cmd);
    
    // Get the device's hardware ID
    static uint64_t getHardwareId();
    
    // Activate device with authorization key
    static bool activate(uint64_t key, uint32_t checksum);
    
    // Deactivate device (clear authorization)
    static void deactivate();
    
private:
    static bool initialized;
    static bool authorized;
    static uint64_t hardwareId;
    
    // Calculate hardware ID from OCOTP registers
    static uint64_t calculateHardwareId();
    
    // Rotate bits left
    static uint64_t rotateLeft(uint64_t value, int bits);
    
    // Validate authorization key
    static bool validateAuth(uint64_t hwId, uint64_t key, uint32_t checksum);
    
    // Handle hidden commands
    static void handleSunbox();
    static void handleMoonrise(const String& keyStr);
    static void handleEclipse();
};

#endif // __cplusplus

#endif // _SUNBOX_AUTH_H_