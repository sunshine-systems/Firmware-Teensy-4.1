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
    // --- Hardcoded constants for movement sanitization ---
    static const uint8_t MOVEMENT_HISTORY_SIZE = 32;
    static const uint8_t MAX_SIGN_FLIPS_PER_SECOND = 8;
    static const uint8_t SENS_RAMP_MS = 32;
    static const uint8_t OUTPUT_RATE_MARGIN_PCT = 10;
    static const int16_t MIN_SPIKE_THRESHOLD = 3;
    static const uint8_t ACCUMULATED_DECAY_MS = 50;  // Discard accumulated serial deltas after 50ms

    // --- Movement sanitization structs ---

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

        // Output history for spike detection (combined output)
        int16_t outputX[MOVEMENT_HISTORY_SIZE];
        int16_t outputY[MOVEMENT_HISTORY_SIZE];
        uint8_t outIndex;
        uint8_t outCount;

        // USB packet rate tracking
        uint32_t usbPacketCount;
        uint32_t windowStartMs;
        uint16_t estimatedUsbRateHz;
    };

    struct SignFlipTracker {
        int8_t lastSign;
        uint8_t flipCount;
        uint32_t windowStartMs;
    };

    struct SensReductionSmoother {
        uint32_t transitionStartMs;
        bool isTransitioning;
        int16_t lastEffectiveReductionX;  // 0-100
        int16_t lastEffectiveReductionY;  // 0-100
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

    // Sensitivity reduction accumulators
    int sensReductionXAccumulator;
    int sensReductionYAccumulator;

    // Spin bot state
    bool spinActive;
    int spinRotationsRemaining;
    unsigned long spinNextMoveTime;
    int spinCurrentX;

    // --- Movement sanitization members ---
    MovementProfileTracker moveProfile;
    SignFlipTracker signFlipX;
    SignFlipTracker signFlipY;
    SensReductionSmoother sensSmooth;

    // Accumulated serial deltas for output rate regulation
    int16_t accumulatedSerialX;
    int16_t accumulatedSerialY;
    uint32_t accumulatedSerialTimestampMs;  // When accumulation started (for decay)

    // Output rate tracking
    uint32_t outputPacketsThisSecond;
    uint32_t outputWindowStartMs;

    // Helper methods
    void outputMouseData(const uint8_t* data, uint32_t length);
    void performButtonFiltering(uint8_t& buttons, uint8_t previousButtons, uint8_t unmodifiedButtons);
    void modifyMovementWithSerialData(int16_t& usbX, int16_t& usbY, int16_t serialX, int16_t serialY);
    void handleSpinBot(uint8_t currentButtons, uint8_t previousButtons, bool isSerialPress);
    void updateSpinBot(int16_t& xMovement, int16_t& yMovement);
    bool shouldExcludeButton(uint8_t currentButtons, uint8_t previousButtons, uint8_t buttonMask);
    void handleMouseButtonConfigCheck(uint8_t& buttons, uint8_t unmodifiedButtons, uint8_t previousButtons,
                                     uint8_t buttonMask, int disablePassthroughOption, unsigned long& lastPressTime);

    // Movement sanitization methods
    void updateMovementProfile(int16_t usbX, int16_t usbY, bool hasUSBData);
    void sanitizeSignFlips(int16_t& finalX, int16_t& finalY, int16_t usbX, int16_t usbY);
    void clampValueSpikes(int16_t& finalX, int16_t& finalY);
    void recordOutput(int16_t finalX, int16_t finalY);
    bool shouldThrottleSerialOnly();

    // Static counters for polling rate measurement
    static uint32_t serialDevicePacketCount;    // S counter
    static uint32_t combinedOutputPacketCount;  // C counter
};

#endif // _SUNBOX_SYNTHETIC_HANDLE_OUTPUT_H_