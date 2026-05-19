#ifndef _ANTI_DETECT_H_
#define _ANTI_DETECT_H_

#include <Arduino.h>

class AntiDetect {
public:
    AntiDetect();
    void begin();

    // Call after combining USB + serial to sanitize the output
    // rawUsbX/Y = the original USB values BEFORE any modification
    // serialX/Y = the serial (aimbot) deltas that were added
    // combinedX/Y = the blended output (usb*sens/100 + serial) — modified in place
    void sanitizeOutput(int16_t rawUsbX, int16_t rawUsbY,
                        int16_t serialX, int16_t serialY,
                        int16_t& combinedX, int16_t& combinedY);

private:
    // Per-axis sign flip prevention
    // When serial would flip the output sign vs raw USB, rejects serial
    // and returns the USB-only scaled value (combined - serial)
    int16_t sanitizeAxis(int16_t rawUsb, int16_t combined, int16_t serial, uint8_t& opposingFrames);

    // State: consecutive opposing frame counters per axis
    uint8_t opposingFramesX;
    uint8_t opposingFramesY;

    // How many consecutive opposing frames before allowing a sign flip
    static const uint8_t OPPOSING_THRESHOLD = 2;
};

#endif
