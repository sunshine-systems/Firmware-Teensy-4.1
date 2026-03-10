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
    blender.sensEffX = 0.0f;   // Aimbot starts at 0% until ramp
    blender.sensEffY = 0.0f;
    blender.rngState = 1;      // Will be re-seeded in begin()

    // Direction-blend state
    blender.ouIntensity = 0.40f;   // OU process starts at mu (stationary)
    blender.sensFloorX = 0.0f;
    blender.sensFloorY = 0.0f;
    blender.prevSteerApplied = false;
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

    // === STEP 1: Always accumulate serial deltas into blender ===
    if (hasSerialData) {
        blender.accumX += (float)serialState.x;
        blender.accumY += (float)serialState.y;
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
        bool sensActive = (enableSensReduction == 1 && millis() <= activationTimestamp4MouseMovementLockout);
        bool hasAccumulator = fabsf(blender.accumX) >= 1.0f || fabsf(blender.accumY) >= 1.0f;
        if (!sensActive || (!hasAccumulator && !hasSerialData)) {
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

    // === STEP 4: Update sensitivity ramp ===
    bool sensActive = (enableSensReduction == 1 && millis() <= activationTimestamp4MouseMovementLockout);
    updateSensitivity(hasSerialData, sensActive);

    // === STEP 5: Flush sub-pixel residuals and detect aimbot state ===
    // Snap accumulator to zero when residual is < 1.0 (can never drain via integer truncation)
    if (fabsf(blender.accumX) < 1.0f) blender.accumX = 0.0f;
    if (fabsf(blender.accumY) < 1.0f) blender.accumY = 0.0f;

    // Force-flush accumulator when it can never meaningfully drain.
    // Cases that cause drain deadlock (accumulator stuck forever):
    //   1. sensActive is false (aim key released)
    //   2. sensActive is true but sensEff < 5.0 on both axes (ramp barely started,
    //      or sensReductionAmmount=100 → target=0%). Without this threshold, the MIN
    //      floor of 1.0 fires at sensEff=0.5 and drains full serial into output even
    //      though the system should be in "user only" mode.
    float sensEffCheckX = calcSensEffective(true);
    float sensEffCheckY = calcSensEffective(false);
    bool canDrain = sensActive && (sensEffCheckX >= 5.0f || sensEffCheckY >= 5.0f);
    if (!canDrain) {
        blender.accumX = 0.0f;
        blender.accumY = 0.0f;
    }

    bool hasAimbot = hasSerialData || fabsf(blender.accumX) >= 1.0f || fabsf(blender.accumY) >= 1.0f;

    // Clean up when aimbot finishes: reset all blender state for clean transition
    if (!hasAimbot && !hasSerialData) {
        if (!blender.sensFirstMovement) {
            // Only reset the sensitivity ramp if sensActive has expired.
            // If sensActive is still true (lockout window open), keep the ramp
            // state so the next serial packet continues from where it left off
            // instead of restarting the sawtooth from 0.
            if (!sensActive) {
                blender.sensFirstMovement = true;
                blender.sensLastSerialMs = 0;
                blender.sensTransitionStartMs = 0;
            }
            // Always clean up sub-pixel and blending state
            blender.spreadAccumX = 0.0f;
            blender.spreadAccumY = 0.0f;
            blender.outputAccumX = 0.0f;
            blender.outputAccumY = 0.0f;
            blender.opposingFramesX = 0;
            blender.opposingFramesY = 0;
            blender.wasIdle = true;
            blender.rampFrame = 0;
            // Reset direction-blend state
            blender.ouIntensity = 0.40f;  // OU back to mu
            blender.sensFloorX = 0.0f;    // Next engagement ramps from 0
            blender.sensFloorY = 0.0f;
            blender.prevSteerApplied = false;
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
        float sensEffX = calcSensEffective(true);
        float sensEffY = calcSensEffective(false);
        logger.infof("SYN:%lu,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
                     millis(),
                     csvRawUsbX, csvRawUsbY,
                     csvRawSerialX, csvRawSerialY,
                     sensActive ? 1 : 0,
                     (int)sensEffX, (int)sensEffY,
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

void SunBoxSyntheticHandleOutput::updateSensitivity(bool hasSerial, bool sensActive) {
    if (!(sensActive && hasSerial)) return;

    uint32_t now = millis();

    if (blender.sensFirstMovement) {
        // First activation — ramp from 0 (sensFloor already 0)
        blender.sensTransitionStartMs = now;
        blender.sensFirstMovement = false;
    } else if (blender.sensLastSerialMs > 0 &&
               (now - blender.sensLastSerialMs) > SENS_RESET_MS) {
        // Gap exceeded reset threshold — snapshot current sensEff as floor
        // before restarting ramp, so it continues from current value
        // instead of dropping to 0 and causing a 200-500ms stall
        blender.sensFloorX = calcSensEffective(true);
        blender.sensFloorY = calcSensEffective(false);
        blender.sensTransitionStartMs = now;
    }

    blender.sensLastSerialMs = now;
}

float SunBoxSyntheticHandleOutput::calcSensEffective(bool isX) {
    // If no serial has arrived yet, aimbot gets nothing
    if (blender.sensFirstMovement) return 0.0f;

    // Check if sens reduction is even active
    bool sensActive = (enableSensReduction == 1 && millis() <= activationTimestamp4MouseMovementLockout);
    if (!sensActive) return 0.0f;

    // Target aimbot fraction from config
    // Config: sensReductionAmmountX=60 means user keeps 60%, aimbot gets 40%
    float target = isX ? (100.0f - (float)sensReductionAmmountX)
                       : (100.0f - (float)sensReductionAmmountY);

    // Transition ramp over SENS_TRANSITION_MS
    uint32_t elapsed = millis() - blender.sensTransitionStartMs;
    float progress = (float)elapsed / (float)SENS_TRANSITION_MS;
    if (progress > 1.0f) progress = 1.0f;

    // Smoothstep: eliminates constant-slope spectral signature of linear ramp.
    // Natural sensitivity changes are fastest mid-transition, tapered at edges.
    progress = progress * progress * (3.0f - 2.0f * progress);

    // Ramp from sensFloor to target instead of 0 to target.
    // On first activation sensFloor is 0 (normal ramp from 0).
    // On mid-engagement gap reset, sensFloor holds the previous sensEff
    // so the ramp continues smoothly instead of dropping to 0.
    float floor = isX ? blender.sensFloorX : blender.sensFloorY;
    return floor + (target - floor) * progress;
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

    if (remaining <= 0.0f || avgSpeed < 0.5f) return 1;

    int spread = (int)ceilf(remaining / avgSpeed);
    if (spread < 1) spread = 1;
    if (spread > 16) spread = 16;
    return spread;
}

float SunBoxSyntheticHandleOutput::drainAccumulator(float available, int spread, bool isX) {
    float remaining = isX ? blender.accumX : blender.accumY;
    if (remaining == 0.0f || available <= 0.0f) return 0.0f;

    float sign = (remaining > 0.0f) ? 1.0f : -1.0f;
    float absRemaining = fabsf(remaining);

    // Target per-frame slice
    float targetSlice = absRemaining / (float)spread;
    float actualSlice = (targetSlice < available) ? targetSlice : available;

    // Sub-pixel accumulator for spread
    float* spreadAccum = isX ? &blender.spreadAccumX : &blender.spreadAccumY;
    float buffered = actualSlice + fabsf(*spreadAccum);
    int drainI = (int)buffered;
    *spreadAccum = (buffered - (float)drainI) * sign;

    if (drainI == 0) return 0.0f;

    // Don't drain more than what's in the accumulator
    if (drainI > (int)absRemaining) drainI = (int)absRemaining;
    if (drainI == 0) return 0.0f;

    if (isX) blender.accumX -= sign * (float)drainI;
    else     blender.accumY -= sign * (float)drainI;

    return sign * (float)drainI;
}

float SunBoxSyntheticHandleOutput::resolveDirection(float userVal, float aimbotVal, bool isX) {
    uint8_t* opposingFrames = isX ? &blender.opposingFramesX : &blender.opposingFramesY;

    // Zero or same direction: just add
    if (userVal == 0.0f || aimbotVal == 0.0f) {
        *opposingFrames = 0;
        return userVal + aimbotVal;
    }

    bool sameSign = (userVal > 0.0f) == (aimbotVal > 0.0f);
    if (sameSign) {
        *opposingFrames = 0;
        return userVal + aimbotVal;
    }

    // Opposing directions
    (*opposingFrames)++;
    float absUser = fabsf(userVal);
    float absAimbot = fabsf(aimbotVal);

    if (absAimbot <= absUser) {
        // Aimbot dampens user but doesn't overcome. Keep user direction.
        float userSign = (userVal > 0.0f) ? 1.0f : -1.0f;
        float dampened = absUser - absAimbot;
        // When aimbot exactly cancels user (result=0), preserve minimum user movement
        // to prevent "sticky cursor" feel at low sensEff where aimbot drains exactly 1
        if (dampened < 0.5f && absUser >= 1.0f) {
            dampened = 0.5f;  // Sub-pixel accumulator will round to 1 over 2 frames
        }
        return userSign * dampened;
    }

    // Aimbot exceeds user. Only flip after 2+ consecutive opposing frames.
    if (*opposingFrames >= 2) {
        float aimbotSign = (aimbotVal > 0.0f) ? 1.0f : -1.0f;
        return aimbotSign * (absAimbot - absUser);
    } else {
        // First opposing frame: cancel to zero, don't flip yet
        return 0.0f;
    }
}

void SunBoxSyntheticHandleOutput::blendMovement(int16_t& outX, int16_t& outY,
                                                  int16_t usbX, int16_t usbY, bool hasAimbot) {
    if (!hasAimbot) {
        return;
    }

    float userSpeed = sqrtf((float)(usbX * usbX + usbY * usbY));
    bool isIdle = moveProfile.avgSpeedX < 1 && moveProfile.avgSpeedY < 1;

    if (userSpeed < 0.5f || isIdle) {
        // === IDLE PATH: burst drain ===
        blendIdleBurst(outX, outY, usbX, usbY);
    } else {
        // === MOVING PATH: direction blend ===
        blendMovingDirection(outX, outY, usbX, usbY, userSpeed);
    }
}

void SunBoxSyntheticHandleOutput::blendIdleBurst(int16_t& outX, int16_t& outY,
                                                   int16_t usbX, int16_t usbY) {
    // Idle path with Ornstein-Uhlenbeck process drain intensity.
    // Replaces the binary burst/pause state machine. The OU process produces
    // temporally correlated drain intensities that match natural micro-movement:
    //   - Positive autocorrelation (clustered bursts, not rhythmic on/off)
    //   - Variable magnitudes (natural speed CV)
    //   - ~30% zero frames (when OU intensity clamps to 0)
    //   - Mean-reverting (prevents drift to extremes)

    // OU parameters (tuned from natural micro-movement recordings)
    const float OU_THETA = 0.10f;   // mean-reversion speed
    const float OU_MU = 0.40f;      // long-term mean intensity
    const float OU_SIGMA = 0.30f;   // volatility

    float mu = OU_MU;

    // Soft accumulator management: boost OU mean when accumulator grows large
    float accumMag = sqrtf(blender.accumX * blender.accumX + blender.accumY * blender.accumY);
    float budgetX = calcBudget(true);
    float budgetY = calcBudget(false);
    blender.lastBudgetX = budgetX;
    blender.lastBudgetY = budgetY;
    float avgBudget = (fabsf(budgetX) + fabsf(budgetY)) / 2.0f;

    if (avgBudget > 0.0f && accumMag > avgBudget * 4.0f) {
        mu += 0.1f;  // gently increase drain rate
    }

    // Step the OU process: dx = theta * (mu - x) * dt + sigma * dW
    float dw = gaussianNoise(1.0f);
    blender.ouIntensity += OU_THETA * (mu - blender.ouIntensity) + OU_SIGMA * dw;

    // Clamp to [0, 1]
    if (blender.ouIntensity < 0.0f) blender.ouIntensity = 0.0f;
    if (blender.ouIntensity > 1.0f) blender.ouIntensity = 1.0f;

    // If OU intensity is near zero, this is a natural pause frame
    if (blender.ouIntensity < 0.01f) {
        outX = usbX;
        outY = usbY;
        return;
    }

    // Square the intensity to widen speed distribution (increases speed CV)
    // while preserving temporal correlation from the OU process.
    // OU=0.8 -> 0.64, OU=0.5 -> 0.25, OU=0.3 -> 0.09
    float effectiveIntensity = blender.ouIntensity * blender.ouIntensity;

    // Apply drain with OU-modulated budget
    outX = usbX;
    outY = usbY;

    for (int axis = 0; axis < 2; axis++) {
        bool isX = (axis == 0);
        float usbVal = isX ? (float)usbX : (float)usbY;

        float budget = isX ? budgetX : budgetY;
        if (budget <= 0.0f) continue;

        float userContrib = usbVal;

        // Scale budget by squared OU intensity
        float aimbotBudget = budget * effectiveIntensity;

        // Ensure minimum drain rate when accumulator has content
        float accumVal = isX ? blender.accumX : blender.accumY;
        if (fabsf(accumVal) >= 1.0f && aimbotBudget < 1.0f) {
            aimbotBudget = 1.0f;
        }

        // Drain accumulator
        int spread = calcSpreadFrames(isX);
        float aimbotContrib = drainAccumulator(aimbotBudget, spread, isX);

        if (aimbotContrib == 0.0f) continue;

        // Direction resolution
        float combined = resolveDirection(userContrib, aimbotContrib, isX);

        // Add noise
        float noiseSigma = fabsf(aimbotBudget) * 0.05f;
        if (noiseSigma < 0.2f) noiseSigma = 0.2f;
        combined += gaussianNoise(noiseSigma);

        // Quantize with sub-pixel accumulator
        float* outputAccum = isX ? &blender.outputAccumX : &blender.outputAccumY;
        float total = combined + *outputAccum;
        int16_t result = (int16_t)total;
        *outputAccum = total - (float)result;

        if (isX) outX = result;
        else     outY = result;
    }
}

void SunBoxSyntheticHandleOutput::blendMovingDirection(int16_t& outX, int16_t& outY,
                                                         int16_t usbX, int16_t usbY, float userSpeed) {
    // Moving path: direction-based vector blending.
    // Steers user's movement vector toward the aimbot target direction
    // while preserving the user's speed magnitude. This maintains
    // speed autocorrelation since output speed ~= input speed.

    // 1. Compute blend_factor from avg sensEff (0..1)
    float sensEffX = calcSensEffective(true);
    float sensEffY = calcSensEffective(false);
    float avgSensEff = (sensEffX + sensEffY) / 2.0f;
    float blendFactor = avgSensEff / 100.0f;
    if (blendFactor > 1.0f) blendFactor = 1.0f;
    if (blendFactor < 0.0f) blendFactor = 0.0f;

    // Compute budgets for diagnostics
    blender.lastBudgetX = calcBudget(true);
    blender.lastBudgetY = calcBudget(false);

    // If no blend, passthrough
    if (blendFactor < 0.001f) {
        outX = usbX;
        outY = usbY;
        return;
    }

    // 2. Get aimbot direction from accumulator
    float accumX = blender.accumX;
    float accumY = blender.accumY;
    float accumMag = sqrtf(accumX * accumX + accumY * accumY);

    if (accumMag < 0.5f) {
        // No meaningful accumulator — passthrough
        outX = usbX;
        outY = usbY;
        return;
    }

    // Aimbot direction unit vector
    float aimDirX = accumX / accumMag;
    float aimDirY = accumY / accumMag;

    // Target vector: aimbot direction at user's speed magnitude
    float targetX = aimDirX * userSpeed;
    float targetY = aimDirY * userSpeed;

    // 3. Stochastic steering: randomly skip steering on some frames to break
    // up acceleration autocorrelation. Anti-correlation: if we steered last
    // frame, slightly less likely to steer this frame (and vice versa).
    float prob = 0.55f;  // base steering probability
    if (blender.prevSteerApplied) {
        prob -= 0.1f;
    } else {
        prob += 0.1f;
    }

    float randVal = fabsf(gaussianNoise(1.0f)) / 3.0f;
    bool applySteer = (randVal < prob);

    if (!applySteer) {
        blender.prevSteerApplied = false;
        outX = usbX;
        outY = usbY;
        return;
    }

    blender.prevSteerApplied = true;

    // 4. Blend: output = user * (1-blend) + target * blend
    float blendedX = (float)usbX * (1.0f - blendFactor) + targetX * blendFactor;
    float blendedY = (float)usbY * (1.0f - blendFactor) + targetY * blendFactor;

    // 5. Compute actual steering amount
    float steerX = blendedX - (float)usbX;
    float steerY = blendedY - (float)usbY;
    float steerMag = sqrtf(steerX * steerX + steerY * steerY);

    // 6. Drain accumulator by the actual steering contribution
    // Drain proportional to how much we steered, projected onto aimbot direction
    float drainAmount = steerX * aimDirX + steerY * aimDirY;  // dot product
    if (drainAmount > 0.0f) {
        float drainX = drainAmount * aimDirX;
        float drainY = drainAmount * aimDirY;

        // Clamp drain to not exceed accumulator
        if (fabsf(drainX) > fabsf(accumX)) drainX = accumX;
        if (fabsf(drainY) > fabsf(accumY)) drainY = accumY;

        blender.accumX -= drainX;
        blender.accumY -= drainY;

        // Snap small accumulators to zero
        if (fabsf(blender.accumX) < 1.0f) blender.accumX = 0.0f;
        if (fabsf(blender.accumY) < 1.0f) blender.accumY = 0.0f;
    }

    // 7. Add small noise proportional to steer magnitude
    if (steerMag > 0.1f) {
        float noiseSigma = steerMag * 0.05f;
        if (noiseSigma < 0.2f) noiseSigma = 0.2f;
        blendedX += gaussianNoise(noiseSigma);
        blendedY += gaussianNoise(noiseSigma);
    }

    // 8. Quantize with sub-pixel accumulators
    float totalX = blendedX + blender.outputAccumX;
    float totalY = blendedY + blender.outputAccumY;
    outX = (int16_t)totalX;
    outY = (int16_t)totalY;
    blender.outputAccumX = totalX - (float)outX;
    blender.outputAccumY = totalY - (float)outY;
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
