#include "SunBoxSyntheticHandleOutput.h"
#include "SunBoxCommands.h"
#include "SunBoxUSBMouseDataHandler.h"
#include "SunBoxLogger.h"
#include "Config.h"
#include "SunBoxStartup.h"

// Static counter initialization
uint32_t SunBoxSyntheticHandleOutput::serialDevicePacketCount = 0;
uint32_t SunBoxSyntheticHandleOutput::combinedOutputPacketCount = 0;

SunBoxSyntheticHandleOutput::SunBoxSyntheticHandleOutput(SunBoxCommands& commands,
                                                       SunBoxUSBMouseDataHandler& usbHandler)
    : commands(commands), usbHandler(usbHandler), usbDeviceProxy(nullptr),
      mouseEndpoint(0), previousUsbButtons(0), previousSerialButtons(0),
      activationTimestamp4MouseButtonExclusion(0), activationTimestamp4MouseMovementLockout(0),
      lastRMBPressTime(0), lastLMBPressTime(0), lastMB4PressTime(0), lastMB5PressTime(0),
      sensReductionXAccumulator(0), sensReductionYAccumulator(0),
      spinActive(false), spinRotationsRemaining(0), spinNextMoveTime(0), spinCurrentX(0),
      accumulatedSerialX(0), accumulatedSerialY(0), accumulatedSerialTimestampMs(0),
      outputPacketsThisSecond(0), outputWindowStartMs(0) {
    previousUsbState.clear();
    previousSerialState.clear();

    // Zero-initialize movement profile tracker
    memset(&moveProfile, 0, sizeof(moveProfile));
    moveProfile.estimatedUsbRateHz = 1000;  // Safe default until measured

    // Zero-initialize sign flip trackers
    memset(&signFlipX, 0, sizeof(signFlipX));
    memset(&signFlipY, 0, sizeof(signFlipY));

    // Zero-initialize sensitivity reduction smoother
    sensSmooth.transitionStartMs = 0;
    sensSmooth.isTransitioning = false;
    sensSmooth.lastEffectiveReductionX = 100;  // Start at full movement (no reduction)
    sensSmooth.lastEffectiveReductionY = 100;
}

void SunBoxSyntheticHandleOutput::begin() {
    // Initialize if needed
}

void SunBoxSyntheticHandleOutput::process() {
    // Check if we have any data to process
    bool hasUSBData = usbHandler.hasData();
    bool hasSerialData = commands.hasData();
    
    // Early exit if no data
    if (!hasUSBData && !hasSerialData) {
        // Still need to process spin bot if active
        if (spinActive) {
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
    
    // Check if USB device proxy is ready
    if (!usbDeviceProxy || !usbDeviceProxy->isConfigured() || mouseEndpoint == 0) {
        // Reset flags to prevent data buildup
        if (hasUSBData) usbHandler.reset();
        if (hasSerialData) commands.resetData();
        return;
    }
    
    // Check if endpoint is ready
    if (!usbDeviceProxy->isEndpointReady(mouseEndpoint)) {
        // Don't reset - let data accumulate for next cycle
        return;
    }
    
    // Get mouse states
    MouseState usbState;
    MouseState serialState;
    MouseState finalState;

    // Track unmodified USB buttons for filtering logic
    uint8_t unmodifiedUsbButtons = 0;

    // Get USB data if available
    if (hasUSBData) {
        const uint8_t* rawData = usbHandler.getRawData();
        uint32_t rawLength = usbHandler.getRawDataLength();
        
        // Parse USB data
        usbHandler.getHIDHandler().parseMouseData(rawData, rawLength, usbState);
        previousUsbButtons = previousUsbState.buttons;
        unmodifiedUsbButtons = usbState.buttons;
        usbHandler.reset();
    } else {
        // Use previous state for buttons (movement doesn't persist)
        usbState.buttons = previousUsbState.buttons;
        previousUsbButtons = previousUsbState.buttons;
        unmodifiedUsbButtons = previousUsbState.buttons;
        usbState.x = 0;
        usbState.y = 0;
        usbState.wheel = 0;
    }
    
    // Get serial data if available
    if (hasSerialData) {
        // Increment serial device packet counter (S)
        serialDevicePacketCount++;

        // Save previous state BEFORE getting new state
        previousSerialButtons = previousSerialState.buttons;

        serialState = commands.getMouseState();
        
        // Check for sensitivity reduction trigger (scroll wheel = 1)
        if (serialState.wheel == 1) {
            activationTimestamp4MouseMovementLockout = millis() + sensReductionDurationMilliseconds;
            // Clear the scroll wheel so it doesn't get sent to host
            serialState.wheel = 0;
        }
        
        // Update previous state for next cycle
        previousSerialState = serialState;
        commands.resetData();
    } else {
        // No serial data this frame - preserve button state
        serialState.clear();
        serialState.buttons = previousSerialState.buttons;  // Preserve last known button state
        // Use saved previous serial buttons
        previousSerialButtons = previousSerialState.buttons;
    }
    
    // Update movement profile from real USB data (before blending)
    updateMovementProfile(usbState.x, usbState.y, hasUSBData);

    // Output rate regulation: if serial-only frame, check if we should throttle
    if (hasSerialData && !hasUSBData) {
        if (shouldThrottleSerialOnly()) {
            // Accumulate serial deltas instead of outputting
            if (accumulatedSerialX == 0 && accumulatedSerialY == 0) {
                accumulatedSerialTimestampMs = millis();  // Record when accumulation started
            }
            accumulatedSerialX += serialState.x;
            accumulatedSerialY += serialState.y;
            // Still update previous state so button tracking isn't lost
            return;
        }
    }

    // Handle accumulated serial deltas: decay or apply smoothly
    if (accumulatedSerialX != 0 || accumulatedSerialY != 0) {
        uint32_t accAge = millis() - accumulatedSerialTimestampMs;

        if (accAge > ACCUMULATED_DECAY_MS) {
            // Too old — discard entirely (stale data would cause a jump)
            accumulatedSerialX = 0;
            accumulatedSerialY = 0;
        } else if (hasUSBData) {
            // Apply smoothly: only release up to the adaptive threshold per frame
            // so we don't dump a big accumulated value all at once
            int16_t maxStepX = moveProfile.avgSpeedX + moveProfile.avgAccelX * 2;
            if (maxStepX < MIN_SPIKE_THRESHOLD) maxStepX = MIN_SPIKE_THRESHOLD;
            int16_t maxStepY = moveProfile.avgSpeedY + moveProfile.avgAccelY * 2;
            if (maxStepY < MIN_SPIKE_THRESHOLD) maxStepY = MIN_SPIKE_THRESHOLD;

            // Release a portion of accumulated deltas, capped by threshold
            int16_t releaseX, releaseY;
            if (accumulatedSerialX > maxStepX) {
                releaseX = maxStepX;
            } else if (accumulatedSerialX < -maxStepX) {
                releaseX = -maxStepX;
            } else {
                releaseX = accumulatedSerialX;
            }
            if (accumulatedSerialY > maxStepY) {
                releaseY = maxStepY;
            } else if (accumulatedSerialY < -maxStepY) {
                releaseY = -maxStepY;
            } else {
                releaseY = accumulatedSerialY;
            }

            serialState.x += releaseX;
            serialState.y += releaseY;
            accumulatedSerialX -= releaseX;
            accumulatedSerialY -= releaseY;
        }
        // If no USB data and not throttled, accumulated deltas wait for next USB frame
    }

    // Handle MMB press for exclusion window
    if (usbState.buttons & MOUSE_MIDDLE) {
        // Update exclusion timestamp
        activationTimestamp4MouseButtonExclusion = millis();
        
        // Check passthrough condition based on disablePassthroughForMMB
        if (disablePassthroughForMMB == 1) {
            // Passthrough disabled, clear the byte
            usbState.buttons &= ~MOUSE_MIDDLE;
        }
    }
    
    // Apply button filtering
    performButtonFiltering(usbState.buttons, previousUsbButtons, unmodifiedUsbButtons);
    
    // Apply button filtering to serial buttons too
    uint8_t unmodifiedSerialButtons = serialState.buttons;
    performButtonFiltering(serialState.buttons, previousSerialButtons, unmodifiedSerialButtons);
    
    // Handle spin bot activation
    bool lmbPressed = !(previousUsbButtons & MOUSE_LEFT) && (usbState.buttons & MOUSE_LEFT);
    bool lmbPressedSerial = !(previousSerialButtons & MOUSE_LEFT) && (serialState.buttons & MOUSE_LEFT);
    if (lmbPressed || lmbPressedSerial) {
        handleSpinBot(usbState.buttons, previousUsbButtons, lmbPressedSerial);
    }
    
    // Combine movement with sensitivity reduction
    int16_t finalX = usbState.x;
    int16_t finalY = usbState.y;
    modifyMovementWithSerialData(finalX, finalY, serialState.x, serialState.y);

    // Sanitize sign flips (Safeguard 1)
    sanitizeSignFlips(finalX, finalY, usbState.x, usbState.y);

    // Clamp value spikes (Safeguard 2)
    clampValueSpikes(finalX, finalY);

    // Update spin bot movement
    updateSpinBot(finalX, finalY);
    
    // Combine buttons using special logic for LMB/RMB
    uint8_t finalButtons = 0;
    
    // Handle each button
    for (uint8_t buttonMask = 1; buttonMask <= 0x10; buttonMask <<= 1) {
        if (buttonMask == MOUSE_LEFT || buttonMask == MOUSE_RIGHT) {
            // Special handling for LMB/RMB - Simple OR logic
            // If both indicate release, release
            if (!(usbState.buttons & buttonMask) && !(serialState.buttons & buttonMask)) {
                // Button is released - don't set the bit
                continue;
            }
            // If either indicate press, press
            if ((usbState.buttons & buttonMask) || (serialState.buttons & buttonMask)) {
                finalButtons |= buttonMask;
            }
        } else {
            // Normal handling for other buttons (MMB, MB4, MB5)
            // Just take the USB state for these buttons (after filtering)
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
    
    // Convert to raw format and send
    uint8_t outputBuffer[64];
    uint32_t outputLength = sizeof(outputBuffer);
    
    // Convert from standard format to device format
    usbHandler.getHIDHandler().formatMouseData(finalState, outputBuffer, outputLength);
    
    // Debug logging to understand the issue
    bool debug_enabled = SunBoxStartup::isDebugEnabled();
    if (debug_enabled && finalState.buttons != 0) {
        logger.debugf("Button state: 0x%02X, Button byte offset: %d", 
                     finalState.buttons, 
                     usbHandler.getHIDHandler().getButtonByteOffset());
        logger.debugf("Output data: %02X %02X %02X %02X %02X %02X %02X %02X",
                     outputLength > 0 ? outputBuffer[0] : 0,
                     outputLength > 1 ? outputBuffer[1] : 0,
                     outputLength > 2 ? outputBuffer[2] : 0,
                     outputLength > 3 ? outputBuffer[3] : 0,
                     outputLength > 4 ? outputBuffer[4] : 0,
                     outputLength > 5 ? outputBuffer[5] : 0,
                     outputLength > 6 ? outputBuffer[6] : 0,
                     outputLength > 7 ? outputBuffer[7] : 0);
    }
    
    // Proper fix: Place buttons at the correct byte position based on HID descriptor
    uint8_t buttonByteOffset = usbHandler.getHIDHandler().getButtonByteOffset();
    if (outputLength > buttonByteOffset) {
        // Only override if the current byte doesn't match expected button state
        // This preserves the work of formatMouseData if it's correct
        if (outputBuffer[buttonByteOffset] != finalState.buttons) {
            outputBuffer[buttonByteOffset] = finalState.buttons;
        }
    }
    
    // Record output for spike detection history
    recordOutput(finalX, finalY);

    // Track output rate for throttling
    outputPacketsThisSecond++;

    // Send the formatted data
    outputMouseData(outputBuffer, outputLength);

    // Update previous state
    if (hasUSBData) {
        previousUsbState = usbState;
        previousUsbState.buttons = unmodifiedUsbButtons; // Store unmodified buttons
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

void SunBoxSyntheticHandleOutput::modifyMovementWithSerialData(int16_t& usbX, int16_t& usbY, int16_t serialX, int16_t serialY) {
    // Convert to long for higher precision calculations
    long usbXLong = static_cast<long>(usbX);
    long usbYLong = static_cast<long>(usbY);

    if (enableSensReduction == 1 && millis() <= activationTimestamp4MouseMovementLockout) {
        // Determine target reduction values
        int16_t targetX = sensReductionAmmountX;
        int16_t targetY = sensReductionAmmountY;

        // Sensitivity reduction ramp (Safeguard 3)
        int16_t effectiveX = targetX;
        int16_t effectiveY = targetY;

        if (targetX != sensSmooth.lastEffectiveReductionX || targetY != sensSmooth.lastEffectiveReductionY) {
            uint32_t now = millis();
            if (!sensSmooth.isTransitioning) {
                // Start a new ramp
                sensSmooth.isTransitioning = true;
                sensSmooth.transitionStartMs = now;
            }

            uint32_t elapsed = now - sensSmooth.transitionStartMs;
            if (elapsed >= SENS_RAMP_MS) {
                // Ramp complete
                effectiveX = targetX;
                effectiveY = targetY;
                sensSmooth.isTransitioning = false;
            } else {
                // Linear ramp: lerp from lastEffective to target over SENS_RAMP_MS
                // Using integer math: result = last + (target - last) * elapsed / SENS_RAMP_MS
                effectiveX = sensSmooth.lastEffectiveReductionX +
                    (int16_t)(((int32_t)(targetX - sensSmooth.lastEffectiveReductionX) * (int32_t)elapsed) / SENS_RAMP_MS);
                effectiveY = sensSmooth.lastEffectiveReductionY +
                    (int16_t)(((int32_t)(targetY - sensSmooth.lastEffectiveReductionY) * (int32_t)elapsed) / SENS_RAMP_MS);
            }
        } else {
            sensSmooth.isTransitioning = false;
        }

        sensSmooth.lastEffectiveReductionX = effectiveX;
        sensSmooth.lastEffectiveReductionY = effectiveY;

        // Apply ramped sensitivity reduction using existing accumulator logic
        if (effectiveX >= 0 && effectiveX <= 100) {
            usbXLong = (usbXLong * effectiveX);
            sensReductionXAccumulator += usbXLong % 100;
            usbXLong = usbXLong / 100;

            // Apply accumulated movement
            if (abs(sensReductionXAccumulator) >= 100) {
                usbX += sensReductionXAccumulator / 100;
                sensReductionXAccumulator %= 100;
            }
        }

        if (effectiveY >= 0 && effectiveY <= 100) {
            usbYLong = (usbYLong * effectiveY);
            sensReductionYAccumulator += usbYLong % 100;
            usbYLong = usbYLong / 100;

            // Apply accumulated movement
            if (abs(sensReductionYAccumulator) >= 100) {
                usbY += sensReductionYAccumulator / 100;
                sensReductionYAccumulator %= 100;
            }
        }

        // Convert back and add serial movement
        usbX = static_cast<int16_t>(usbXLong + serialX);
        usbY = static_cast<int16_t>(usbYLong + serialY);
    } else {
        // No sensitivity reduction active - ramp back to 100 (full movement)
        int16_t targetX = 100;
        int16_t targetY = 100;

        if (targetX != sensSmooth.lastEffectiveReductionX || targetY != sensSmooth.lastEffectiveReductionY) {
            uint32_t now = millis();
            if (!sensSmooth.isTransitioning) {
                sensSmooth.isTransitioning = true;
                sensSmooth.transitionStartMs = now;
            }

            uint32_t elapsed = now - sensSmooth.transitionStartMs;
            if (elapsed >= SENS_RAMP_MS) {
                sensSmooth.lastEffectiveReductionX = 100;
                sensSmooth.lastEffectiveReductionY = 100;
                sensSmooth.isTransitioning = false;
            } else {
                int16_t effectiveX = sensSmooth.lastEffectiveReductionX +
                    (int16_t)(((int32_t)(targetX - sensSmooth.lastEffectiveReductionX) * (int32_t)elapsed) / SENS_RAMP_MS);
                int16_t effectiveY = sensSmooth.lastEffectiveReductionY +
                    (int16_t)(((int32_t)(targetY - sensSmooth.lastEffectiveReductionY) * (int32_t)elapsed) / SENS_RAMP_MS);

                sensSmooth.lastEffectiveReductionX = effectiveX;
                sensSmooth.lastEffectiveReductionY = effectiveY;

                // Apply the ramped (not yet full) reduction
                usbXLong = (usbXLong * effectiveX) / 100;
                usbYLong = (usbYLong * effectiveY) / 100;

                usbX = static_cast<int16_t>(usbXLong + serialX);
                usbY = static_cast<int16_t>(usbYLong + serialY);
                return;
            }
        } else {
            sensSmooth.isTransitioning = false;
        }

        // No sensitivity reduction, just add serial movement
        usbX += serialX;
        usbY += serialY;
    }
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
    for (uint8_t i = 0; i < moveProfile.histCount; i++) {
        int16_t ax = moveProfile.usbDeltaX[i] < 0 ? -moveProfile.usbDeltaX[i] : moveProfile.usbDeltaX[i];
        int16_t ay = moveProfile.usbDeltaY[i] < 0 ? -moveProfile.usbDeltaY[i] : moveProfile.usbDeltaY[i];
        sumAbsX += ax;
        sumAbsY += ay;
        if (ax > maxAbsX) maxAbsX = ax;
        if (ay > maxAbsY) maxAbsY = ay;
    }
    moveProfile.avgSpeedX = (int16_t)(sumAbsX / moveProfile.histCount);
    moveProfile.avgSpeedY = (int16_t)(sumAbsY / moveProfile.histCount);
    moveProfile.maxDeltaX = maxAbsX;
    moveProfile.maxDeltaY = maxAbsY;

    // Recompute avgAccelX/Y = mean of abs(consecutive differences)
    if (moveProfile.histCount >= 2) {
        int32_t sumAccelX = 0;
        int32_t sumAccelY = 0;
        uint8_t pairs = moveProfile.histCount - 1;
        for (uint8_t i = 0; i < pairs; i++) {
            // For a ring buffer, we iterate over valid sequential entries
            // Since histIndex points to next write position, valid entries are
            // arranged with oldest at (histIndex - histCount) and newest at (histIndex - 1)
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

bool SunBoxSyntheticHandleOutput::shouldThrottleSerialOnly() {
    uint32_t now = millis();

    // Reset output rate window every second
    if (now - outputWindowStartMs >= 1000) {
        outputPacketsThisSecond = 0;
        outputWindowStartMs = now;
    }

    // Check if outputting would exceed estimatedUsbRateHz * 1.1
    // Using integer math: rate * (100 + margin) / 100
    uint32_t maxRate = (uint32_t)moveProfile.estimatedUsbRateHz * (100 + OUTPUT_RATE_MARGIN_PCT) / 100;
    return outputPacketsThisSecond >= maxRate;
}

void SunBoxSyntheticHandleOutput::sanitizeSignFlips(int16_t& finalX, int16_t& finalY, int16_t usbX, int16_t usbY) {
    uint32_t now = millis();

    // --- X axis ---
    {
        // Reset window if >1000ms elapsed
        if (now - signFlipX.windowStartMs >= 1000) {
            signFlipX.flipCount = 0;
            signFlipX.windowStartMs = now;
        }

        int8_t curSign = (finalX > 0) ? 1 : ((finalX < 0) ? -1 : 0);

        if (curSign != 0 && signFlipX.lastSign != 0 && curSign != signFlipX.lastSign) {
            signFlipX.flipCount++;

            if (signFlipX.flipCount > MAX_SIGN_FLIPS_PER_SECOND) {
                // Check if USB alone would NOT have flipped (serial caused it)
                int8_t usbSign = (usbX > 0) ? 1 : ((usbX < 0) ? -1 : 0);
                if (usbSign == 0 || usbSign == signFlipX.lastSign) {
                    // Serial caused the flip, revert to USB-only
                    finalX = usbX;
                    curSign = usbSign;
                }
            }
        }

        if (curSign != 0) {
            signFlipX.lastSign = curSign;
        }
    }

    // --- Y axis ---
    {
        if (now - signFlipY.windowStartMs >= 1000) {
            signFlipY.flipCount = 0;
            signFlipY.windowStartMs = now;
        }

        int8_t curSign = (finalY > 0) ? 1 : ((finalY < 0) ? -1 : 0);

        if (curSign != 0 && signFlipY.lastSign != 0 && curSign != signFlipY.lastSign) {
            signFlipY.flipCount++;

            if (signFlipY.flipCount > MAX_SIGN_FLIPS_PER_SECOND) {
                int8_t usbSign = (usbY > 0) ? 1 : ((usbY < 0) ? -1 : 0);
                if (usbSign == 0 || usbSign == signFlipY.lastSign) {
                    finalY = usbY;
                    curSign = usbSign;
                }
            }
        }

        if (curSign != 0) {
            signFlipY.lastSign = curSign;
        }
    }
}

void SunBoxSyntheticHandleOutput::clampValueSpikes(int16_t& finalX, int16_t& finalY) {
    // Need at least 2 output history entries
    if (moveProfile.outCount < 2) return;

    // Get previous output value (most recent entry)
    uint8_t prevIdx = (moveProfile.outIndex == 0) ? (MOVEMENT_HISTORY_SIZE - 1) : (moveProfile.outIndex - 1);
    int16_t prevOutX = moveProfile.outputX[prevIdx];
    int16_t prevOutY = moveProfile.outputY[prevIdx];

    // Adaptive threshold per axis: max(MIN_SPIKE_THRESHOLD, avgSpeed + avgAccel * 2)
    int16_t threshX = moveProfile.avgSpeedX + moveProfile.avgAccelX * 2;
    if (threshX < MIN_SPIKE_THRESHOLD) threshX = MIN_SPIKE_THRESHOLD;

    int16_t threshY = moveProfile.avgSpeedY + moveProfile.avgAccelY * 2;
    if (threshY < MIN_SPIKE_THRESHOLD) threshY = MIN_SPIKE_THRESHOLD;

    // Clamp X
    int16_t diffX = finalX - prevOutX;
    if (diffX > threshX) {
        finalX = prevOutX + threshX;
    } else if (diffX < -threshX) {
        finalX = prevOutX - threshX;
    }

    // Clamp Y
    int16_t diffY = finalY - prevOutY;
    if (diffY > threshY) {
        finalY = prevOutY + threshY;
    } else if (diffY < -threshY) {
        finalY = prevOutY - threshY;
    }
}

void SunBoxSyntheticHandleOutput::recordOutput(int16_t finalX, int16_t finalY) {
    moveProfile.outputX[moveProfile.outIndex] = finalX;
    moveProfile.outputY[moveProfile.outIndex] = finalY;
    moveProfile.outIndex = (moveProfile.outIndex + 1) % MOVEMENT_HISTORY_SIZE;
    if (moveProfile.outCount < MOVEMENT_HISTORY_SIZE) {
        moveProfile.outCount++;
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