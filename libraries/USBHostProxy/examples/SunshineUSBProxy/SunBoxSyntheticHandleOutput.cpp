#include "SunBoxSyntheticHandleOutput.h"
#include "SunBoxCommands.h"
#include "SunBoxUSBMouseDataHandler.h"
#include "SunBoxLogger.h"
#include "Config.h"
#include "SunBoxStartup.h"
#include <math.h>

// Static counter initialization
uint32_t SunBoxSyntheticHandleOutput::serialDevicePacketCount = 0;
uint32_t SunBoxSyntheticHandleOutput::combinedOutputPacketCount = 0;

SunBoxSyntheticHandleOutput::SunBoxSyntheticHandleOutput(SunBoxCommands& commands,
                                                       SunBoxUSBMouseDataHandler& usbHandler)
    : commands(commands), usbHandler(usbHandler), usbDeviceProxy(nullptr),
      mouseEndpoint(0), previousUsbButtons(0), previousSerialButtons(0),
      activationTimestamp4MouseButtonExclusion(0), activationTimestamp4MouseMovementLockout(0),
      lastRMBPressTime(0), lastLMBPressTime(0), lastMB4PressTime(0), lastMB5PressTime(0),
      spinActive(false), spinRotationsRemaining(0), spinNextMoveTime(0), spinCurrentX(0),
      lastOutputMs(0) {
    previousUsbState.clear();
    previousSerialState.clear();

    // Zero-initialize movement profile tracker
    memset(&moveProfile, 0, sizeof(moveProfile));
    moveProfile.estimatedUsbRateHz = 1000;  // Safe default until measured

    // Zero-initialize blender
    memset(&blender, 0, sizeof(blender));
    blender.wasIdle = true;
    blender.sensFirstMovement = true;
    blender.rngState = 1;      // Will be re-seeded in begin()

    // OU process starts at mu (stationary)
    blender.ouIntensity = 0.40f;
}

void SunBoxSyntheticHandleOutput::begin() {
    blender.rngState = micros();  // Non-reproducible seed
    if (blender.rngState == 0) blender.rngState = 42;  // LCG can't have 0 state
}

void SunBoxSyntheticHandleOutput::process() {
    bool hasUSBData = usbHandler.hasData();
    bool hasSerialData = commands.hasData();

    // Early exit if no data (spin bot exception)
    if (!hasUSBData && !hasSerialData) {
        if (spinActive) {
            // Spin bot can still output (cosmetic, not aimbot)
            MouseState emptyState;
            emptyState.clear();
            int16_t xMove = 0, yMove = 0;
            updateSpinBot(xMove, yMove);
            if (xMove != 0 || yMove != 0) {
                emptyState.x = xMove;
                emptyState.y = yMove;
                uint8_t outputBuffer[64];
                uint32_t outputLength = sizeof(outputBuffer);
                usbHandler.getHIDHandler().formatMouseData(emptyState, outputBuffer, outputLength);
                outputMouseData(outputBuffer, outputLength);
            }
        }
        return;
    }

    // Proxy readiness check
    if (!usbDeviceProxy || !usbDeviceProxy->isConfigured() || mouseEndpoint == 0) {
        if (hasUSBData) usbHandler.reset();
        if (hasSerialData) commands.resetData();
        return;
    }

    // Endpoint readiness check
    if (!usbDeviceProxy->isEndpointReady(mouseEndpoint)) {
        return;
    }

    MouseState usbState;
    MouseState serialState;
    MouseState finalState;
    uint8_t unmodifiedUsbButtons = 0;

    // Parse USB data
    if (hasUSBData) {
        const uint8_t* rawData = usbHandler.getRawData();
        uint32_t rawLength = usbHandler.getRawDataLength();
        usbHandler.getHIDHandler().parseMouseData(rawData, rawLength, usbState);
        previousUsbButtons = previousUsbState.buttons;
        unmodifiedUsbButtons = usbState.buttons;
        usbHandler.reset();
    } else {
        usbState.buttons = previousUsbState.buttons;
        previousUsbButtons = previousUsbState.buttons;
        unmodifiedUsbButtons = previousUsbState.buttons;
        usbState.x = 0;
        usbState.y = 0;
        usbState.wheel = 0;
    }

    // Parse serial data
    if (hasSerialData) {
        serialDevicePacketCount++;
        previousSerialButtons = previousSerialState.buttons;
        serialState = commands.getMouseState();

        // Sensitivity reduction trigger (scroll wheel = 1)
        if (serialState.wheel == 1) {
            activationTimestamp4MouseMovementLockout = millis() + sensReductionDurationMilliseconds;
            serialState.wheel = 0;
        }

        previousSerialState = serialState;
        commands.resetData();
    } else {
        serialState.clear();
        serialState.buttons = previousSerialState.buttons;
        previousSerialButtons = previousSerialState.buttons;
    }

    // Capture raw values for logging
    int16_t csvRawUsbX = usbState.x;
    int16_t csvRawUsbY = usbState.y;
    int16_t csvRawSerialX = hasSerialData ? serialState.x : 0;
    int16_t csvRawSerialY = hasSerialData ? serialState.y : 0;

    // === STEP 1: Accumulate serial deltas into blender, capped by drain capacity ===
    if (hasSerialData) {
        blender.accumX += (float)serialState.x;
        blender.accumY += (float)serialState.y;
        blender.sensLastSerialMs = millis();
        blender.sensFirstMovement = false;

        // Cap accumulator to what can drain in ~32 frames (the spread window).
        // With the credit system, same-direction user movement drains the accum
        // fast, so a generous cap is fine. This just prevents extreme accumulation
        // when serial floods in with no USB frames to drain against.
        for (int axis = 0; axis < 2; axis++) {
            bool isX = (axis == 0);
            float* accum = isX ? &blender.accumX : &blender.accumY;
            float budget = isX ? blender.lastBudgetX : blender.lastBudgetY;
            if (budget < 3.0f) budget = 3.0f;
            float accumCap = budget * 32.0f;
            if (accumCap < 16.0f) accumCap = 16.0f;
            if (fabsf(*accum) > accumCap) {
                float sign = (*accum > 0.0f) ? 1.0f : -1.0f;
                *accum = sign * accumCap;
            }
        }
    }

    // === STEP 2: Handle missing USB frames ===
    // During active mouse movement, we piggyback on USB frames (prevents C > R anomaly).
    // When mouse is idle but aimbot is active (sensActive + accumulator), we output
    // paced at the mouse's polling rate — looks like the mouse "woke up."
    // CRITICAL: Only generate serial-only output when sensActive is true.
    // Without this gate, serial arriving between USB frames inserts (0,0) output frames
    // (sensEff=0 → aimbot drains nothing, USB=0 → no user movement) which creates
    // a stutter pattern: real movement, zero, real movement, zero.
    if (!hasUSBData) {
        bool sensActiveLocal = (enableSensReduction == 1 && millis() <= activationTimestamp4MouseMovementLockout);
        bool recentSerialLocal = blender.sensLastSerialMs > 0 &&
                                 (millis() - blender.sensLastSerialMs) <= SENS_RESET_MS;
        bool hasAccumulator = fabsf(blender.accumX) >= 1.0f || fabsf(blender.accumY) >= 1.0f;
        if (!(sensActiveLocal || recentSerialLocal) || (!hasAccumulator && !hasSerialData)) {
            return;  // Nothing useful to output — don't insert zero frames
        }
        // Pace serial-only output at the mouse's polling rate
        uint32_t now = millis();
        uint16_t rate = moveProfile.estimatedUsbRateHz;
        if (rate < 125) rate = 125;  // Floor at 125Hz (safe minimum)
        uint32_t intervalMs = 1000 / rate;
        if (intervalMs < 1) intervalMs = 1;
        if ((now - lastOutputMs) < intervalMs) {
            return;  // Too soon — wait for next poll interval
        }
        // Proceed with serial-only output (usbState already zeroed above)
    }

    // === STEP 3: Update movement profile from real USB data ===
    // Only update profile when we have actual USB data (don't pollute with zeros)
    updateMovementProfile(usbState.x, usbState.y, hasUSBData);

    // === STEP 4: Sensitivity active check ===
    bool sensActive = (enableSensReduction == 1 && millis() <= activationTimestamp4MouseMovementLockout);

    // === STEP 5: Flush sub-pixel residuals and detect aimbot state ===
    // Snap accumulator to zero when residual is < 1.0 (can never drain via integer truncation)
    if (fabsf(blender.accumX) < 1.0f) blender.accumX = 0.0f;
    if (fabsf(blender.accumY) < 1.0f) blender.accumY = 0.0f;

    // Force-flush accumulator after genuine serial gap.
    // With sensReductionDuration=0, sensActive expires within 1ms of each trigger.
    // Don't flush accumulator just because sensActive is momentarily false between
    // serial packets — only flush after a genuine serial gap (>250ms).
    bool recentSerial = blender.sensLastSerialMs > 0 &&
                        (millis() - blender.sensLastSerialMs) <= SENS_RESET_MS;
    if (!sensActive && !recentSerial) {
        blender.accumX = 0.0f;
        blender.accumY = 0.0f;
    }

    bool hasAimbot = hasSerialData || fabsf(blender.accumX) >= 1.0f || fabsf(blender.accumY) >= 1.0f;

    // Clean up when aimbot finishes: reset all blender state for clean transition.
    // Use serial gap check instead of sensActive — with sensReductionDuration=0,
    // sensActive expires within 1ms of each trigger, causing false cleanups between
    // serial packets that are only 5-12ms apart. Only reset after a genuine gap.
    if (!hasAimbot && !hasSerialData) {
        if (!blender.sensFirstMovement) {
            bool serialGap = blender.sensLastSerialMs == 0 ||
                             (millis() - blender.sensLastSerialMs) > SENS_RESET_MS;
            if (!sensActive && serialGap) {
                blender.sensFirstMovement = true;
                blender.sensLastSerialMs = 0;
            }
            // Always clean up sub-pixel and blending state
            blender.spreadAccumX = 0.0f;
            blender.spreadAccumY = 0.0f;
            blender.outputAccumX = 0.0f;
            blender.outputAccumY = 0.0f;
            blender.wasIdle = true;
            blender.rampFrame = 0;
            blender.ouIntensity = 0.40f;
        }
    }

    // === STEP 6: Blend movement ===
    int16_t finalX = usbState.x;
    int16_t finalY = usbState.y;

    // Increment idle ramp frame ONCE per frame (outside per-axis loop)
    if (hasAimbot) {
        bool isIdle = moveProfile.avgSpeedX < 1 && moveProfile.avgSpeedY < 1 && moveProfile.histCount >= 2;
        if (isIdle && (blender.accumX != 0.0f || blender.accumY != 0.0f)) {
            if (!blender.wasIdle) {
                blender.wasIdle = true;
                blender.rampFrame = 0;
            }
            if (blender.rampFrame < IDLE_RAMP_FRAMES) {
                blender.rampFrame++;
            }
        } else if (!isIdle && blender.wasIdle) {
            blender.wasIdle = false;
            blender.rampFrame = 0;
        }
        blendMovement(finalX, finalY, usbState.x, usbState.y, hasAimbot);
    }

    // === STEP 7: Spin bot (cosmetic) ===
    updateSpinBot(finalX, finalY);

    // === STEP 8: Handle MMB and button filtering (UNCHANGED from original) ===
    if (usbState.buttons & MOUSE_MIDDLE) {
        activationTimestamp4MouseButtonExclusion = millis();
        if (disablePassthroughForMMB == 1) {
            usbState.buttons &= ~MOUSE_MIDDLE;
        }
    }
    performButtonFiltering(usbState.buttons, previousUsbButtons, unmodifiedUsbButtons);
    uint8_t unmodifiedSerialButtons = serialState.buttons;
    performButtonFiltering(serialState.buttons, previousSerialButtons, unmodifiedSerialButtons);

    // Spin bot activation check
    bool lmbPressed = !(previousUsbButtons & MOUSE_LEFT) && (usbState.buttons & MOUSE_LEFT);
    bool lmbPressedSerial = !(previousSerialButtons & MOUSE_LEFT) && (serialState.buttons & MOUSE_LEFT);
    if (lmbPressed || lmbPressedSerial) {
        handleSpinBot(usbState.buttons, previousUsbButtons, lmbPressedSerial);
    }

    // === STEP 9: Combine buttons (UNCHANGED logic) ===
    uint8_t finalButtons = 0;
    for (uint8_t buttonMask = 1; buttonMask <= 0x10; buttonMask <<= 1) {
        if (buttonMask == MOUSE_LEFT || buttonMask == MOUSE_RIGHT) {
            if ((usbState.buttons & buttonMask) || (serialState.buttons & buttonMask)) {
                finalButtons |= buttonMask;
            }
        } else {
            if (usbState.buttons & buttonMask) {
                finalButtons |= buttonMask;
            }
        }
    }

    // Build final state
    finalState.buttons = finalButtons;
    finalState.x = finalX;
    finalState.y = finalY;
    finalState.wheel = usbState.wheel + serialState.wheel;

    // Format output
    uint8_t outputBuffer[64];
    uint32_t outputLength = sizeof(outputBuffer);
    usbHandler.getHIDHandler().formatMouseData(finalState, outputBuffer, outputLength);

    // Button byte correction
    uint8_t buttonByteOffset = usbHandler.getHIDHandler().getButtonByteOffset();
    if (outputLength > buttonByteOffset) {
        if (outputBuffer[buttonByteOffset] != finalState.buttons) {
            outputBuffer[buttonByteOffset] = finalState.buttons;
        }
    }

    // === STEP 10: Log when aimbot active (including drain frames) ===
    if (hasSerialData || blender.accumX != 0.0f || blender.accumY != 0.0f) {
        logger.infof("SYN:%lu,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
                     millis(),
                     csvRawUsbX, csvRawUsbY,
                     csvRawSerialX, csvRawSerialY,
                     sensActive ? 1 : 0,
                     (int)sensReductionAmmountX, (int)sensReductionAmmountY,
                     finalX, finalY,
                     (int)blender.lastBudgetX, (int)blender.lastBudgetY,
                     (int)blender.accumX, (int)blender.accumY);
    }

    // Send output and track timing for serial-only pacing
    outputMouseData(outputBuffer, outputLength);
    lastOutputMs = millis();

    // Update previous state
    if (hasUSBData) {
        previousUsbState = usbState;
        previousUsbState.buttons = unmodifiedUsbButtons;
    }
}

void SunBoxSyntheticHandleOutput::outputMouseData(const uint8_t* data, uint32_t length) {
    if (usbDeviceProxy && mouseEndpoint > 0) {
        // Increment combined output packet counter (C)
        combinedOutputPacketCount++;
        usbDeviceProxy->sendDataOnEndpoint(mouseEndpoint, data, length);
    }
}

void SunBoxSyntheticHandleOutput::performButtonFiltering(uint8_t& buttons, uint8_t previousButtons, uint8_t unmodifiedButtons) {
    // Apply filtering for each button based on settings
    handleMouseButtonConfigCheck(buttons, unmodifiedButtons, previousButtons, MOUSE_RIGHT, disablePassthroughForRMB, lastRMBPressTime);
    handleMouseButtonConfigCheck(buttons, unmodifiedButtons, previousButtons, MOUSE_LEFT, disablePassthroughForLMB, lastLMBPressTime);
    handleMouseButtonConfigCheck(buttons, unmodifiedButtons, previousButtons, MOUSE_BUTTON4, disablePassthroughForMB4, lastMB4PressTime);
    handleMouseButtonConfigCheck(buttons, unmodifiedButtons, previousButtons, MOUSE_BUTTON5, disablePassthroughForMB5, lastMB5PressTime);
}

void SunBoxSyntheticHandleOutput::updateMovementProfile(int16_t usbX, int16_t usbY, bool hasUSBData) {
    if (!hasUSBData) return;

    // Record USB deltas in ring buffer
    moveProfile.usbDeltaX[moveProfile.histIndex] = usbX;
    moveProfile.usbDeltaY[moveProfile.histIndex] = usbY;
    moveProfile.histIndex = (moveProfile.histIndex + 1) % MOVEMENT_HISTORY_SIZE;
    if (moveProfile.histCount < MOVEMENT_HISTORY_SIZE) {
        moveProfile.histCount++;
    }

    // Recompute avgSpeedX/Y = mean of abs values over history
    int32_t sumAbsX = 0;
    int32_t sumAbsY = 0;
    int16_t maxAbsX = 0;
    int16_t maxAbsY = 0;
    int32_t sumSqAbsX = 0;
    int32_t sumSqAbsY = 0;
    for (uint8_t i = 0; i < moveProfile.histCount; i++) {
        int16_t ax = moveProfile.usbDeltaX[i] < 0 ? -moveProfile.usbDeltaX[i] : moveProfile.usbDeltaX[i];
        int16_t ay = moveProfile.usbDeltaY[i] < 0 ? -moveProfile.usbDeltaY[i] : moveProfile.usbDeltaY[i];
        sumAbsX += ax;
        sumAbsY += ay;
        sumSqAbsX += (int32_t)ax * ax;
        sumSqAbsY += (int32_t)ay * ay;
        if (ax > maxAbsX) maxAbsX = ax;
        if (ay > maxAbsY) maxAbsY = ay;
    }
    moveProfile.avgSpeedX = (int16_t)(sumAbsX / moveProfile.histCount);
    moveProfile.avgSpeedY = (int16_t)(sumAbsY / moveProfile.histCount);
    moveProfile.maxDeltaX = maxAbsX;
    moveProfile.maxDeltaY = maxAbsY;

    // stddev = sqrt(E[X^2] - E[X]^2)
    int32_t varianceX = (sumSqAbsX / moveProfile.histCount) - (int32_t)moveProfile.avgSpeedX * moveProfile.avgSpeedX;
    int32_t varianceY = (sumSqAbsY / moveProfile.histCount) - (int32_t)moveProfile.avgSpeedY * moveProfile.avgSpeedY;
    if (varianceX < 0) varianceX = 0;  // Rounding protection
    if (varianceY < 0) varianceY = 0;
    moveProfile.speedStddevX = (int16_t)sqrtf((float)varianceX);
    moveProfile.speedStddevY = (int16_t)sqrtf((float)varianceY);

    // Recompute avgAccelX/Y = mean of abs(consecutive differences)
    if (moveProfile.histCount >= 2) {
        int32_t sumAccelX = 0;
        int32_t sumAccelY = 0;
        uint8_t pairs = moveProfile.histCount - 1;
        for (uint8_t i = 0; i < pairs; i++) {
            uint8_t ci = (moveProfile.histIndex - moveProfile.histCount + i) % MOVEMENT_HISTORY_SIZE;
            uint8_t ni = (ci + 1) % MOVEMENT_HISTORY_SIZE;
            int16_t diffX = moveProfile.usbDeltaX[ni] - moveProfile.usbDeltaX[ci];
            int16_t diffY = moveProfile.usbDeltaY[ni] - moveProfile.usbDeltaY[ci];
            sumAccelX += (diffX < 0 ? -diffX : diffX);
            sumAccelY += (diffY < 0 ? -diffY : diffY);
        }
        moveProfile.avgAccelX = (int16_t)(sumAccelX / pairs);
        moveProfile.avgAccelY = (int16_t)(sumAccelY / pairs);
    } else {
        moveProfile.avgAccelX = 0;
        moveProfile.avgAccelY = 0;
    }

    // Track USB packet rate
    uint32_t now = millis();
    moveProfile.usbPacketCount++;
    if (now - moveProfile.windowStartMs >= 1000) {
        moveProfile.estimatedUsbRateHz = (uint16_t)moveProfile.usbPacketCount;
        moveProfile.usbPacketCount = 0;
        moveProfile.windowStartMs = now;
    }
}

float SunBoxSyntheticHandleOutput::gaussianNoise(float sigma) {
    if (sigma <= 0.0f) return 0.0f;

    // LCG: generate two uniform random numbers in (0, 1)
    blender.rngState = blender.rngState * 1664525u + 1013904223u;
    float u1 = (float)(blender.rngState >> 1) / 2147483648.0f;  // (0, 1)
    if (u1 < 1e-10f) u1 = 1e-10f;  // Protect log(0)

    blender.rngState = blender.rngState * 1664525u + 1013904223u;
    float u2 = (float)(blender.rngState >> 1) / 2147483648.0f;

    // Box-Muller transform
    float z = sqrtf(-2.0f * logf(u1)) * cosf(6.2831853f * u2);
    return z * sigma;
}

float SunBoxSyntheticHandleOutput::calcBudget(bool isX) {
    float avgSpeed = isX ? (float)moveProfile.avgSpeedX : (float)moveProfile.avgSpeedY;
    float avgAccel = isX ? (float)moveProfile.avgAccelX : (float)moveProfile.avgAccelY;
    float stddev = isX ? (float)moveProfile.speedStddevX : (float)moveProfile.speedStddevY;

    bool isIdle = moveProfile.avgSpeedX < 1 && moveProfile.avgSpeedY < 1 && moveProfile.histCount >= 2;

    if (isIdle) {
        float accum = isX ? blender.accumX : blender.accumY;
        if (accum == 0.0f) return 0.0f;

        // Idle ramp: sqrt acceleration curve
        // rampFrame is incremented once per frame in process(), not here
        uint8_t frame = blender.rampFrame;
        if (frame < 1) frame = 1;

        float base = 2.5f * sqrtf((float)frame);
        float sigma = base * 0.25f;
        if (sigma < 0.4f) sigma = 0.4f;
        float budget = base + gaussianNoise(sigma);
        if (budget < 1.0f) budget = 1.0f;

        // Cap at reasonable human onset speed
        float cap = 15.0f + gaussianNoise(1.5f);
        if (cap < 3.0f) cap = 3.0f;
        if (budget > cap) budget = cap;

        return budget;
    }

    // Moving state is managed in process(), not here

    // Adaptive budget from profile + gaussian noise proportional to speed_stddev
    float base = avgSpeed + avgAccel * 2.0f;
    float sigma = stddev * 0.3f;
    // Ensure minimum budget variance even during low-velocity movement.
    // Without this, budget locks at a constant value (e.g., 3) for many consecutive
    // frames, which is detectable — natural movement has gaussian-distributed speeds.
    float minVariance = base * 0.15f;  // At least 15% of base as noise
    if (sigma < minVariance) sigma = minVariance;
    float budget = base + gaussianNoise(sigma);
    if (budget < (float)MIN_SPIKE_THRESHOLD) budget = (float)MIN_SPIKE_THRESHOLD;

    return budget;
}

int SunBoxSyntheticHandleOutput::calcSpreadFrames(bool isX) {
    float remaining = fabsf(isX ? blender.accumX : blender.accumY);
    float avgSpeed = isX ? (float)moveProfile.avgSpeedX : (float)moveProfile.avgSpeedY;

    bool isIdle = moveProfile.avgSpeedX < 1 && moveProfile.avgSpeedY < 1;
    int minSpread = isIdle ? MIN_SPREAD_IDLE : MIN_SPREAD_MOVING;

    if (remaining <= 0.0f || avgSpeed < 0.5f) return minSpread;

    float effectiveSpeed = avgSpeed;
    if (effectiveSpeed < 1.0f) effectiveSpeed = 1.0f;
    int spread = (int)ceilf(remaining / effectiveSpeed);
    if (spread < minSpread) spread = minSpread;
    if (spread > 16) spread = 16;
    return spread;
}

float SunBoxSyntheticHandleOutput::drainAxis(bool isX, float drainAmount) {
    float accumVal = isX ? blender.accumX : blender.accumY;
    if (accumVal == 0.0f || drainAmount <= 0.0f) return 0.0f;

    float sign = (accumVal > 0.0f) ? 1.0f : -1.0f;
    float absRemaining = fabsf(accumVal);

    // Sub-pixel accumulator for smooth drain
    float* spreadAccum = isX ? &blender.spreadAccumX : &blender.spreadAccumY;
    float buffered = drainAmount + fabsf(*spreadAccum);
    int drainI = (int)buffered;
    *spreadAccum = (buffered - (float)drainI) * sign;

    if (drainI == 0) return 0.0f;

    // Don't drain more than available
    if (drainI > (int)absRemaining) drainI = (int)absRemaining;
    if (drainI == 0) return 0.0f;

    float drainVal = sign * (float)drainI;
    if (isX) blender.accumX -= drainVal;
    else     blender.accumY -= drainVal;

    return drainVal;
}

float SunBoxSyntheticHandleOutput::scaleUsbAxis(int16_t usbVal, bool isX) {
    if (usbVal == 0) return 0.0f;

    // sensReductionAmmount = how much user input to keep (0=lockout, 100=full movement).
    // Software ramps this from initial (e.g. 0) upward to give user back control.
    //   sensReductionAmmountX=0   → full lockout   → scale = 0.00
    //   sensReductionAmmountX=70  → user keeps 70% → scale = 0.70
    //   sensReductionAmmountX=100 → full movement  → scale = 1.00
    float amount = isX ? (float)sensReductionAmmountX : (float)sensReductionAmmountY;
    float scale = amount / 100.0f;
    if (scale < 0.0f) scale = 0.0f;
    if (scale > 1.0f) scale = 1.0f;

    return (float)usbVal * scale;
}

int SunBoxSyntheticHandleOutput::combineAndQuantize(float scaledUsb, float drainVal, bool isX) {
    float combined = scaledUsb + drainVal;

    // Add noise proportional to drain magnitude
    if (drainVal != 0.0f) {
        float noiseSigma = fabsf(drainVal) * 0.05f;
        if (noiseSigma < 0.2f) noiseSigma = 0.2f;
        combined += gaussianNoise(noiseSigma);
    }

    // Quantize with output sub-pixel accumulator
    float* outputAccum = isX ? &blender.outputAccumX : &blender.outputAccumY;
    float total = combined + *outputAccum;
    int16_t result = (int16_t)total;
    *outputAccum = total - (float)result;

    return (int)result;
}

void SunBoxSyntheticHandleOutput::blendMovement(int16_t& outX, int16_t& outY,
                                                  int16_t usbX, int16_t usbY, bool hasAimbot) {
    if (!hasAimbot) return;

    float userSpeed = sqrtf((float)(usbX * usbX + usbY * usbY));
    bool isIdle = moveProfile.avgSpeedX < 1 && moveProfile.avgSpeedY < 1;

    float fOutX, fOutY;
    if (userSpeed < 0.5f && isIdle) {
        blendIdle(fOutX, fOutY, usbX, usbY);
    } else {
        blendMoving(fOutX, fOutY, usbX, usbY);
    }

    outX = (int16_t)fOutX;
    outY = (int16_t)fOutY;
}

void SunBoxSyntheticHandleOutput::blendIdle(float& outX, float& outY,
                                              int16_t usbX, int16_t usbY) {
    // Idle path: OU process modulates drain RATE but never pauses
    // when accumulator has content. Serial is spread over future USB frames.

    const float OU_THETA = 0.10f;
    const float OU_MU = 0.40f;
    const float OU_SIGMA = 0.30f;

    float mu = OU_MU;

    // Soft accumulator management: boost OU mean when accumulator is large
    float accumMag = sqrtf(blender.accumX * blender.accumX + blender.accumY * blender.accumY);
    if (accumMag > 20.0f) {
        mu += 0.1f;
    }

    // Step the OU process
    float dw = gaussianNoise(1.0f);
    blender.ouIntensity += OU_THETA * (mu - blender.ouIntensity) + OU_SIGMA * dw;
    if (blender.ouIntensity < 0.0f) blender.ouIntensity = 0.0f;
    if (blender.ouIntensity > 1.0f) blender.ouIntensity = 1.0f;

    // Compute budget for diagnostics
    float budgetX = calcBudget(true);
    float budgetY = calcBudget(false);
    blender.lastBudgetX = budgetX;
    blender.lastBudgetY = budgetY;

    bool hasAccum = fabsf(blender.accumX) >= 1.0f || fabsf(blender.accumY) >= 1.0f;

    // Scale USB
    float scaledX = scaleUsbAxis(usbX, true);
    float scaledY = scaleUsbAxis(usbY, false);

    // If no accumulator, return scaled USB (natural pause frame is OK)
    if (!hasAccum) {
        outX = scaledX;
        outY = scaledY;
        // Quantize fractional USB through output accum
        outX = (float)combineAndQuantize(scaledX, 0.0f, true);
        outY = (float)combineAndQuantize(scaledY, 0.0f, false);
        return;
    }

    // OU modulates drain rate — squared intensity for natural speed CV
    float effectiveIntensity = blender.ouIntensity * blender.ouIntensity;
    // Floor: always drain at least some when accumulator has content
    if (effectiveIntensity < 0.15f) effectiveIntensity = 0.15f;

    outX = scaledX;
    outY = scaledY;

    // Per-axis: drain and combine
    for (int axis = 0; axis < 2; axis++) {
        bool isX = (axis == 0);
        float accumVal = isX ? blender.accumX : blender.accumY;

        if (accumVal == 0.0f) {
            // Quantize fractional scaled USB
            float sv = isX ? outX : outY;
            int qv = combineAndQuantize(sv, 0.0f, isX);
            if (isX) outX = (float)qv;
            else     outY = (float)qv;
            continue;
        }

        float absRemaining = fabsf(accumVal);

        // Drain rate: intensity-modulated, spread over frames
        int spread = calcSpreadFrames(isX);
        float drainTarget = (absRemaining / (float)spread) * effectiveIntensity;

        // Always at least 1 count when accumulator has content
        if (drainTarget < 1.0f && absRemaining >= 1.0f) drainTarget = 1.0f;

        // Small gaussian variation for naturalness
        drainTarget += gaussianNoise(drainTarget * 0.1f);
        if (drainTarget < 1.0f && absRemaining >= 1.0f) drainTarget = 1.0f;

        float drainVal = drainAxis(isX, drainTarget);
        float sv = isX ? outX : outY;
        int qv = combineAndQuantize(sv, drainVal, isX);
        if (isX) outX = (float)qv;
        else     outY = (float)qv;
    }
}

void SunBoxSyntheticHandleOutput::blendMoving(float& outX, float& outY,
                                                int16_t usbX, int16_t usbY) {
    // Moving path: two behaviors depending on whether user agrees with aimbot.
    //
    // SAME DIRECTION (user moving where aimbot wants):
    //   Credit the user's raw movement against the accumulator — the user is
    //   already doing the aimbot's work. Output scaled USB only (no drain added).
    //   This lets the aimbot "hide" behind the user's natural movement.
    //
    // OPPOSITE DIRECTION (user fights aimbot):
    //   Drain from accumulator, but limit drain so it can never flip the user's
    //   movement direction. At high sens (user keeps 80%+), drain is tiny.

    // Compute budgets (for smoothness cap and logging)
    blender.lastBudgetX = calcBudget(true);
    blender.lastBudgetY = calcBudget(false);

    // Scale USB by sensitivity reduction
    float scaledX = scaleUsbAxis(usbX, true);
    float scaledY = scaleUsbAxis(usbY, false);

    outX = scaledX;
    outY = scaledY;

    // Per-axis: credit or drain
    for (int axis = 0; axis < 2; axis++) {
        bool isX = (axis == 0);
        float accumVal = isX ? blender.accumX : blender.accumY;

        if (accumVal == 0.0f) {
            // No aimbot pending — just quantize scaled USB
            float sv = isX ? outX : outY;
            int qv = combineAndQuantize(sv, 0.0f, isX);
            if (isX) outX = (float)qv;
            else     outY = (float)qv;
            continue;
        }

        int16_t rawUsb = isX ? usbX : usbY;
        float scaledUsb = isX ? scaledX : scaledY;
        float absAccum = fabsf(accumVal);
        float accumSign = (accumVal > 0.0f) ? 1.0f : -1.0f;

        // Check if user is moving in the same direction as the accumulator
        bool sameDirection = (accumVal > 0.0f && rawUsb > 0) ||
                             (accumVal < 0.0f && rawUsb < 0);

        if (sameDirection) {
            // === CREDIT PATH ===
            // User naturally moving where aimbot wants — credit their raw
            // movement against the accumulator. Output scaled USB only.
            float credit = (float)(rawUsb < 0 ? -rawUsb : rawUsb);
            if (credit > absAccum) credit = absAccum;

            // Deduct credit from accumulator
            if (isX) blender.accumX -= accumSign * credit;
            else     blender.accumY -= accumSign * credit;

            // Output scaled USB only (sens reduction applies, no drain needed)
            int qv = combineAndQuantize(scaledUsb, 0.0f, isX);
            if (isX) outX = (float)qv;
            else     outY = (float)qv;
        } else {
            // === DRAIN PATH (opposite direction or user idle on this axis) ===
            // Spread is the only smoother — no budget cap. This matches the old
            // code's behavior where serial was added in full each frame. The spread
            // distributes it over MIN_SPREAD_MOVING (3) frames for anti-detection,
            // but the budget cap on top created persistent drag that felt "stuck"
            // (budget=3 at low speeds meant 3-5 frame drain for a -10 serial impulse,
            // when the old code applied it in one frame).
            int spread = calcSpreadFrames(isX);
            float drainTarget = absAccum / (float)spread;

            // Minimum drain of 1 when accumulator has content
            if (drainTarget < 1.0f && absAccum >= 1.0f) drainTarget = 1.0f;

            // Small gaussian variation for naturalness
            if (drainTarget > 0.0f) {
                drainTarget += gaussianNoise(drainTarget * 0.1f);
                if (drainTarget < 0.0f) drainTarget = 0.0f;
            }

            float drainVal = drainAxis(isX, drainTarget);
            float sv = isX ? outX : outY;
            int qv = combineAndQuantize(sv, drainVal, isX);
            if (isX) outX = (float)qv;
            else     outY = (float)qv;
        }
    }
}

void SunBoxSyntheticHandleOutput::handleSpinBot(uint8_t currentButtons, uint8_t previousButtons, bool isSerialPress) {
    // Check if spinning is enabled
    if (enableSpinning == 0) {
        return;
    }

    // Check timing preference
    bool shouldSpinBefore = (spinBeforeAfterMouseEvent == 0 || spinBeforeAfterMouseEvent == 2);
    bool shouldSpinAfter = (spinBeforeAfterMouseEvent == 1 || spinBeforeAfterMouseEvent == 2);

    if (shouldSpinBefore) {
        // Start spin immediately
        spinActive = true;
        spinRotationsRemaining = spinNumberOfRotations;
        spinNextMoveTime = millis();
        spinCurrentX = 0;
    } else if (shouldSpinAfter) {
        // Delay spin by 5ms
        spinActive = true;
        spinRotationsRemaining = spinNumberOfRotations;
        spinNextMoveTime = millis() + 5;
        spinCurrentX = 0;
    }
}

void SunBoxSyntheticHandleOutput::updateSpinBot(int16_t& xMovement, int16_t& yMovement) {
    if (!spinActive || spinRotationsRemaining <= 0) {
        spinActive = false;
        return;
    }

    // Check if it's time for next rotation
    if (millis() >= spinNextMoveTime) {
        xMovement += spinAmountPerRotation;
        spinRotationsRemaining--;

        if (spinRotationsRemaining > 0) {
            spinNextMoveTime = millis() + spinDelayBetweenRotationsMilliseconds;
        } else {
            spinActive = false;
        }
    }
}

bool SunBoxSyntheticHandleOutput::shouldExcludeButton(uint8_t currentButtons, uint8_t previousButtons, uint8_t buttonMask) {
    return (currentButtons & buttonMask) && !(previousButtons & buttonMask) &&
           (millis() - activationTimestamp4MouseButtonExclusion) <= BUTTON_EXCLUSION_DURATION_MS;
}

void SunBoxSyntheticHandleOutput::handleMouseButtonConfigCheck(uint8_t& buttons, uint8_t unmodifiedButtons,
                                                              uint8_t previousButtons, uint8_t buttonMask,
                                                              int disablePassthroughOption, unsigned long& lastPressTime) {
    unsigned long currentTime = millis();

    if (unmodifiedButtons & buttonMask) {
        // Button is pressed
        if (disablePassthroughOption == 0) {
            // Case 0: Normal passthrough
        } else if (disablePassthroughOption == 1) {
            // Case 1: Block passthrough
            buttons &= ~buttonMask;
        } else if (disablePassthroughOption == 2) {
            // Case 2: Block based on MMB timing
            if (shouldExcludeButton(unmodifiedButtons, previousButtons, buttonMask)) {
                buttons &= ~buttonMask;
            }
        } else if (disablePassthroughOption == 3) {
            // Case 3: Double-tap passthrough logic
            if (!(previousButtons & buttonMask) && (unmodifiedButtons & buttonMask)) {
                // New press
                if (lastPressTime && (currentTime - lastPressTime <= BUTTON_DOUBLE_TAP_TO_PASSTHROUGH_DURATION_MS)) {
                    // Double-tap detected
                    lastPressTime = 0;
                } else {
                    // First press or timeout
                    buttons &= ~buttonMask;
                    lastPressTime = currentTime;
                }
            } else if ((previousButtons & buttonMask) && (unmodifiedButtons & buttonMask)) {
                // Held down
                if (lastPressTime == 0) {
                    // Double-tap was detected, maintain passthrough
                } else {
                    // No double-tap, block
                    buttons &= ~buttonMask;
                }
            }

            // Also check MMB exclusion
            if (shouldExcludeButton(unmodifiedButtons, previousButtons, buttonMask)) {
                buttons &= ~buttonMask;
            }
        }
    } else if (previousButtons & buttonMask) {
        // Button released
        lastPressTime = currentTime;
    }
}
