#ifndef _SUNBOX_SYNTHETIC_HANDLE_OUTPUT_H_
#define _SUNBOX_SYNTHETIC_HANDLE_OUTPUT_H_

#include <Arduino.h>
#include "HIDMouseDescriptorHandler.h"
#include "USBDeviceProxy.h"

// Mouse button definitions (matching Arduino)
#define MOUSE_LEFT      0x1
#define MOUSE_RIGHT     0x2
#define MOUSE_MIDDLE    0x4
#define MOUSE_BUTTON4   0x8
#define MOUSE_BUTTON5   0x10

// Forward declarations
class SunBoxCommands;
class SunBoxUSBMouseDataHandler;

class SunBoxSyntheticHandleOutput {
public:
    SunBoxSyntheticHandleOutput(SunBoxCommands& commands,
                               SunBoxUSBMouseDataHandler& usbHandler);

    // Initialize
    void begin();

    // Set USB Device Proxy reference
    void setUSBDeviceProxy(USBDeviceProxy* proxy) { usbDeviceProxy = proxy; }

    // Set mouse endpoint for output
    void setMouseEndpoint(uint8_t ep) { mouseEndpoint = ep; }

    // Process data from both sources and output
    void process();

    // Get and reset serial device packet counter (S)
    static uint32_t getSerialDeviceCount() { uint32_t count = serialDevicePacketCount; serialDevicePacketCount = 0; return count; }

    // Get and reset combined output packet counter (C)
    static uint32_t getCombinedOutputCount() { uint32_t count = combinedOutputPacketCount; combinedOutputPacketCount = 0; return count; }

private:
    // --- Hardcoded constants ---
    static const uint8_t MOVEMENT_HISTORY_SIZE = 32;
    static const uint8_t SENS_RAMP_MS = 32;
    static const int16_t MIN_SPIKE_THRESHOLD = 3;
    static const uint8_t IDLE_RAMP_FRAMES = 75;
    static const uint16_t SENS_TRANSITION_MS = 200;
    static const uint16_t SENS_RESET_MS = 250;

    // --- Movement profile tracking ---

    struct MovementProfileTracker {
        // Real USB movement history (ring buffer)
        int16_t usbDeltaX[MOVEMENT_HISTORY_SIZE];
        int16_t usbDeltaY[MOVEMENT_HISTORY_SIZE];
        uint8_t histIndex;
        uint8_t histCount;

        // Derived metrics (updated each frame)
        int16_t avgSpeedX;
        int16_t avgSpeedY;
        int16_t avgAccelX;
        int16_t avgAccelY;
        int16_t maxDeltaX;
        int16_t maxDeltaY;

        // Speed standard deviation (for variable budget)
        int16_t speedStddevX;
        int16_t speedStddevY;

        // USB packet rate tracking
        uint32_t usbPacketCount;
        uint32_t windowStartMs;
        uint16_t estimatedUsbRateHz;
    };

    struct AimbotBlender {
        // Accumulator — ALL serial deltas go here, drained over multiple frames
        float accumX;
        float accumY;

        // Spread sub-pixel accumulators (fractional drain remainder)
        float spreadAccumX;
        float spreadAccumY;

        // Output sub-pixel accumulators (final quantization remainder)
        float outputAccumX;
        float outputAccumY;

        // Direction conflict counters (consecutive opposing frames before allowing flip)
        uint8_t opposingFramesX;
        uint8_t opposingFramesY;

        // Idle ramp state
        bool wasIdle;
        uint8_t rampFrame;

        // Sensitivity transition ramp state
        uint32_t sensTransitionStartMs;
        uint32_t sensLastSerialMs;
        bool sensFirstMovement;
        float sensEffX;  // Current effective aimbot fraction (0-100)
        float sensEffY;

        // RNG state for gaussian noise (LCG seed)
        uint32_t rngState;

        // Last computed budget (cached for logging, avoids re-calling calcBudget)
        float lastBudgetX;
        float lastBudgetY;

        // --- Direction-blend state ---

        // OU-process drain intensity for idle path (replaces burst/pause)
        float ouIntensity;         // current OU process value [0, 1]

        // sensEff ramp continuity (snapshot on gap reset)
        float sensFloorX;          // sensEff value at time of last gap reset
        float sensFloorY;

        // Stochastic steering state for moving path
        bool prevSteerApplied;     // anti-correlation tracking
    };

    // References
    SunBoxCommands& commands;
    SunBoxUSBMouseDataHandler& usbHandler;
    USBDeviceProxy* usbDeviceProxy;

    // Configuration
    uint8_t mouseEndpoint;

    // State tracking for USB
    MouseState previousUsbState;
    uint8_t previousUsbButtons;

    // State tracking for Serial
    MouseState previousSerialState;
    uint8_t previousSerialButtons;

    // Timestamps for button events
    unsigned long activationTimestamp4MouseButtonExclusion;
    unsigned long activationTimestamp4MouseMovementLockout;
    unsigned long lastRMBPressTime;
    unsigned long lastLMBPressTime;
    unsigned long lastMB4PressTime;
    unsigned long lastMB5PressTime;

    // Spin bot state
    bool spinActive;
    int spinRotationsRemaining;
    unsigned long spinNextMoveTime;
    int spinCurrentX;

    // --- Movement blending members ---
    MovementProfileTracker moveProfile;
    AimbotBlender blender;

    // Serial-only output pacing (output at USB polling rate when mouse is idle)
    uint32_t lastOutputMs;

    // Helper methods
    void outputMouseData(const uint8_t* data, uint32_t length);
    void performButtonFiltering(uint8_t& buttons, uint8_t previousButtons, uint8_t unmodifiedButtons);
    void handleSpinBot(uint8_t currentButtons, uint8_t previousButtons, bool isSerialPress);
    void updateSpinBot(int16_t& xMovement, int16_t& yMovement);
    bool shouldExcludeButton(uint8_t currentButtons, uint8_t previousButtons, uint8_t buttonMask);
    void handleMouseButtonConfigCheck(uint8_t& buttons, uint8_t unmodifiedButtons, uint8_t previousButtons,
                                     uint8_t buttonMask, int disablePassthroughOption, unsigned long& lastPressTime);

    // Movement profile methods
    void updateMovementProfile(int16_t usbX, int16_t usbY, bool hasUSBData);

    // New blending pipeline methods
    void blendMovement(int16_t& outX, int16_t& outY, int16_t usbX, int16_t usbY, bool hasAimbot);
    void blendIdleBurst(int16_t& outX, int16_t& outY, int16_t usbX, int16_t usbY);
    void blendMovingDirection(int16_t& outX, int16_t& outY, int16_t usbX, int16_t usbY, float userSpeed);
    float calcBudget(bool isX);
    float drainAccumulator(float available, int spread, bool isX);
    float resolveDirection(float userVal, float aimbotVal, bool isX);
    int calcSpreadFrames(bool isX);
    void updateSensitivity(bool hasSerial, bool sensActive);
    float calcSensEffective(bool isX);
    float gaussianNoise(float sigma);

    // Static counters for polling rate measurement
    static uint32_t serialDevicePacketCount;    // S counter
    static uint32_t combinedOutputPacketCount;  // C counter
};

#endif // _SUNBOX_SYNTHETIC_HANDLE_OUTPUT_H_
