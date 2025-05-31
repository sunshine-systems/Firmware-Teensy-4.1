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

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
// C++ class definition
class SunBoxStartup {
public:
    static void begin();      // Just initializes Serial4
    static bool isReady();    // Returns true after begin()
    
private:
    static bool initialized;
    static bool ready;
};
#endif

#endif // _SUNBOX_STARTUP_H_