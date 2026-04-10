#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

/* Define the version */
#define FIRMWARE_VERSION 5.17

/* Settings for logging via Serial4 */
#define DEBUG_MODE false // Set to 'false' when you don't want to print debug info.

/* Sets the timeframe that will be used to exclude side thumb buttons from being passed to the PC when MMB is clicked */
const unsigned long BUTTON_EXCLUSION_DURATION_MS = 1200; // Exclusion Timeframe within button activation (1200 = 1.2sec)

/* Sets the timeframe the user has to double tap a button and have it pass through to the host pc*/
const unsigned long BUTTON_DOUBLE_TAP_TO_PASSTHROUGH_DURATION_MS = 350;

// Setting that gets toggled during serial read for performance metrics analysis
extern bool logPerformanceMetrics; // Configurable setting to enable or disable logging on serial events
extern bool logAPerformanceMetric; // Enables itself when serial events are available

/* BELOW ARE OPERATION SETTINGS LIKE MOUSE LOCKOUT AND HARDWARE SPINBOT */
extern int enableSensReduction; // 0 = Disabled, 1 = Enabled
extern int sensReductionDurationMilliseconds;
extern int sensReductionAmmountX; // 100 = full movement (no reduction), 0 = full lockout (full reduction)
extern int sensReductionAmmountY; // 100 = full movement (no reduction), 0 = full lockout (full reduction)
extern int enableSpinning; // 0=Disabled, 1=Enabled
extern int spinAmountPerRotation;
extern int spinNumberOfRotations;
extern int spinDelayBetweenRotationsMilliseconds;
extern bool spinLockoutMouseUntilCompletion;
extern int spinBeforeAfterMouseEvent; // 0=Before, 1=After, 2=Both
extern int disablePassthroughForMMB; //0 = False, 1 = True
extern int disablePassthroughForRMB; //0 = False, 1 = True, 2 = Only if MMB has been pressed within duration, 3 = Only pass through if double tapped (and or held) within duration
extern int disablePassthroughForLMB; //0 = False, 1 = True, 2 = Only if MMB has been pressed within duration, 3 = Only pass through if double tapped (and or held) within duration
extern int disablePassthroughForMB4; //0 = False, 1 = True, 2 = Only if MMB has been pressed within duration, 3 = Only pass through if double tapped (and or held) within duration
extern int disablePassthroughForMB5; //0 = False, 1 = True, 2 = Only if MMB has been pressed within duration, 3 = Only pass through if double tapped (and or held) within duration

#endif