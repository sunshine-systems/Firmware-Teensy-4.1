// SunBoxStartup.h
#ifndef _SUNBOX_STARTUP_H_
#define _SUNBOX_STARTUP_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// C-callable functions for startup.c
void SunBoxStartup_begin(void);
uint8_t SunBoxStartup_isReady(void);
uint8_t SunBoxStartup_isDebugEnabled(void);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
// C++ class definition
class SunBoxStartup {
public:
    static void begin();      // Initializes Serial4 and loads EEPROM settings
    static bool isReady();    // Returns true after begin()
    static bool isDebugEnabled(); // Returns debug mode state from EEPROM
    static void setDebugEnabled(bool enabled); // Update debug state (for runtime changes)
    
private:
    static bool initialized;
    static bool ready;
    static bool debugEnabled;
};
#endif

#endif // _SUNBOX_STARTUP_H_