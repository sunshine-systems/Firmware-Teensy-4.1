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
    static const uint16_t SENS_RESET_MS = 250;

    // v8: minimum spread frames — serial distributed over at least this many USB frames
    static const uint8_t MIN_SPREAD_IDLE = 4;
    static const uint8_t MIN_SPREAD_MOVING = 3;

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

        // Idle ramp state
        bool wasIdle;
        uint8_t rampFrame;

        // Serial timing (for recentSerial checks and cleanup gating)
        uint32_t sensLastSerialMs;
        bool sensFirstMovement;

        // RNG state for gaussian noise (LCG seed)
        uint32_t rngState;

        // Last computed budget (cached for logging, avoids re-calling calcBudget)
        float lastBudgetX;
        float lastBudgetY;

        // OU-process drain intensity for idle path
        float ouIntensity;         // current OU process value [0, 1]

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

    // v8 blending pipeline — simple USB scaling + serial drain
    void blendMovement(int16_t& outX, int16_t& outY, int16_t usbX, int16_t usbY, bool hasAimbot);
    void blendIdle(float& outX, float& outY, int16_t usbX, int16_t usbY);
    void blendMoving(float& outX, float& outY, int16_t usbX, int16_t usbY);
    float scaleUsbAxis(int16_t usbVal, bool isX);
    float drainAxis(bool isX, float drainAmount);
    int combineAndQuantize(float scaledUsb, float drainVal, bool isX);
    float calcBudget(bool isX);
    int calcSpreadFrames(bool isX);
    float gaussianNoise(float sigma);

    // Static counters for polling rate measurement
    static uint32_t serialDevicePacketCount;    // S counter
    static uint32_t combinedOutputPacketCount;  // C counter
};

#endif // _SUNBOX_SYNTHETIC_HANDLE_OUTPUT_H_
