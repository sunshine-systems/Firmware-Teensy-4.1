#include "AntiDetect.h"

AntiDetect::AntiDetect()
    : opposingFramesX(0)
    , opposingFramesY(0) {
}

void AntiDetect::begin() {
    opposingFramesX = 0;
    opposingFramesY = 0;
}

void AntiDetect::sanitizeOutput(int16_t rawUsbX, int16_t rawUsbY,
                                 int16_t serialX, int16_t serialY,
                                 int16_t& combinedX, int16_t& combinedY) {
    combinedX = sanitizeAxis(rawUsbX, combinedX, serialX, opposingFramesX);
    combinedY = sanitizeAxis(rawUsbY, combinedY, serialY, opposingFramesY);
}

int16_t AntiDetect::sanitizeAxis(int16_t rawUsb, int16_t combined, int16_t serial, uint8_t& opposingFrames) {
    // No serial contribution this frame — nothing to sanitize
    if (serial == 0) {
        opposingFrames = 0;
        return combined;
    }

    // USB is zero — no user direction to protect, allow serial through
    if (rawUsb == 0) {
        opposingFrames = 0;
        return combined;
    }

    // Combined is zero — no conflict possible
    if (combined == 0) {
        opposingFrames = 0;
        return combined;
    }

    // Check if the combined output sign matches the raw USB sign
    bool sameSign = (rawUsb > 0) == (combined > 0);

    if (sameSign) {
        // Serial didn't flip the sign — no conflict, pass through
        opposingFrames = 0;
        return combined;
    }

    // OPPOSING: serial has flipped the output sign vs user's USB direction
    opposingFrames++;

    int16_t absSerial = abs(serial);
    int16_t absScaledUsb = abs(combined - serial);  // recover the USB-only scaled value

    if (opposingFrames < OPPOSING_THRESHOLD) {
        if (absSerial <= absScaledUsb) {
            // Aimbot weaker than user: dampen user's movement but keep user's direction
            // Output = user direction at reduced magnitude (user - aimbot)
            int16_t userSign = (rawUsb > 0) ? 1 : -1;
            int16_t dampened = absScaledUsb - absSerial;
            // Floor at 1 count to avoid creating artificial zero frames
            if (dampened < 1 && absScaledUsb >= 1) {
                dampened = 1;
            }
            return userSign * dampened;
        } else {
            // Aimbot stronger than user: cancel to zero (can't preserve user direction
            // without outputting a tiny value that looks unnatural)
            return 0;
        }
    }

    // After OPPOSING_THRESHOLD consecutive frames: allow the flip
    // Sustained aimbot correction (e.g., user overshooting) needs to eventually land
    return combined;
}
