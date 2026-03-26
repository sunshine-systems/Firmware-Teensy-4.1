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
      deltaFrameCount(0) {
    memset(deltaBufferX, 0, sizeof(deltaBufferX));
    memset(deltaBufferY, 0, sizeof(deltaBufferY));
    previousUsbState.clear();
    previousSerialState.clear();
}

void SunBoxSyntheticHandleOutput::begin() {
    // Initialize if needed
    antiDetect.begin();
}

void SunBoxSyntheticHandleOutput::process() {
    // Check if we have any data to process
    bool hasUSBData = usbHandler.hasData();
    bool hasSerialData = commands.hasData();

    // Early exit if no data
    if (!hasUSBData && !hasSerialData) {
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

    // Capture raw values for SYN logging BEFORE any modification
    int16_t csvRawUsbX = usbState.x;
    int16_t csvRawUsbY = usbState.y;
    int16_t csvRawSerialX = hasSerialData ? serialState.x : 0;
    int16_t csvRawSerialY = hasSerialData ? serialState.y : 0;

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

    // Combine movement with sensitivity reduction
    int16_t finalX = usbState.x;
    int16_t finalY = usbState.y;
    modifyMovementWithSerialData(finalX, finalY, serialState.x, serialState.y);

    // Anti-detection: sanitize sign flips caused by serial input
    if (hasSerialData) {
        antiDetect.sanitizeOutput(csvRawUsbX, csvRawUsbY,
                                  serialState.x, serialState.y,
                                  finalX, finalY);
    }

    // Combine buttons using handoff state machine for LMB/RMB
    uint8_t finalButtons = 0;

    // Handle each button
    for (uint8_t buttonMask = 1; buttonMask <= 0x10; buttonMask <<= 1) {
        if (buttonMask == MOUSE_LEFT) {
            finalButtons |= processButtonHandoff(lmbHandoff, buttonMask,
                                                 serialState.buttons, usbState.buttons,
                                                 previousUsbButtons);
        } else if (buttonMask == MOUSE_RIGHT) {
            finalButtons |= processButtonHandoff(rmbHandoff, buttonMask,
                                                 serialState.buttons, usbState.buttons,
                                                 previousUsbButtons);
        } else {
            // Normal handling for other buttons (MMB, MB4, MB5)
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
        if (outputBuffer[buttonByteOffset] != finalState.buttons) {
            outputBuffer[buttonByteOffset] = finalState.buttons;
        }
    }

    // SYN logging — 14-field format for anti-detection analysis
    // SYN:<millis>,<rawUsbX>,<rawUsbY>,<rawSerialX>,<rawSerialY>,<sensActive>,
    //     <sensReductionAmmountX>,<sensReductionAmmountY>,<finalX>,<finalY>,
    //     <budgetX>,<budgetY>,<accumX>,<accumY>
    // Budget and accum are 0 in this simple firmware — reserved for analysis pipeline compatibility
    if (hasSerialData) {
        bool sensActive = (enableSensReduction == 1 && millis() <= activationTimestamp4MouseMovementLockout);
        logger.infof("SYN:%lu,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
                     millis(),
                     csvRawUsbX, csvRawUsbY,
                     csvRawSerialX, csvRawSerialY,
                     sensActive ? 1 : 0,
                     (int)sensReductionAmmountX, (int)sensReductionAmmountY,
                     finalX, finalY,
                     0, 0,   // budgetX, budgetY (N/A in simple firmware)
                     0, 0);  // accumX, accumY (N/A in simple firmware)
    }

    // Track last output button state
    lastOutputButtons = finalState.buttons;

    // Send the formatted data
    outputMouseData(outputBuffer, outputLength);

    // Buffer raw USB deltas for M: output (every 10 USB frames)
    if (hasUSBData) {
        deltaBufferX[deltaFrameCount] = csvRawUsbX;
        deltaBufferY[deltaFrameCount] = csvRawUsbY;
        deltaFrameCount++;

        if (deltaFrameCount >= DELTA_BUFFER_SIZE) {
            // Format: M: x,y:x,y:x,y:... (10 pairs)
            char buf[128];
            int pos = 0;
            for (uint8_t i = 0; i < DELTA_BUFFER_SIZE; i++) {
                if (i > 0) buf[pos++] = ':';
                pos += snprintf(buf + pos, sizeof(buf) - pos, "%d,%d",
                               deltaBufferX[i], deltaBufferY[i]);
            }
            logger.mouse(buf);
            deltaFrameCount = 0;
        }
    }

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

uint8_t SunBoxSyntheticHandleOutput::processButtonHandoff(
    ButtonHandoff& handoff, uint8_t buttonMask,
    uint8_t serialButtons, uint8_t usbButtons, uint8_t prevUsbButtons) {

    bool synHeld = (serialButtons & buttonMask) != 0;
    bool usbHeld = (usbButtons & buttonMask) != 0;
    bool usbEdge = usbHeld && !(prevUsbButtons & buttonMask);  // Rising edge on USB

    switch (handoff.state) {
        case HANDOFF_IDLE:
            if (synHeld && !usbHeld) {
                handoff.state = HANDOFF_SYNTHETIC_HOLD;
                return buttonMask;  // Pressed (synthetic controls)
            }
            // Normal OR logic when idle
            return (synHeld || usbHeld) ? buttonMask : 0;

        case HANDOFF_SYNTHETIC_HOLD:
            if (!synHeld) {
                // Synthetic released normally
                handoff.state = HANDOFF_IDLE;
                return usbHeld ? buttonMask : 0;
            }
            if (usbEdge) {
                // User pressed while synthetic holding — begin handoff
                handoff.state = HANDOFF_RELEASE;
                handoff.releaseStartMs = millis();
                handoff.gapDurationMs = HANDOFF_GAP_MIN_MS +
                    (random(0, HANDOFF_GAP_MAX_MS - HANDOFF_GAP_MIN_MS + 1));
                return 0;  // Force release
            }
            return buttonMask;  // Still held by synthetic

        case HANDOFF_RELEASE:
            if (millis() - handoff.releaseStartMs >= handoff.gapDurationMs) {
                // Gap elapsed, hand control to user
                handoff.state = HANDOFF_USER_CONTROL;
                return usbHeld ? buttonMask : 0;
            }
            return 0;  // Maintain forced release during gap

        case HANDOFF_USER_CONTROL:
            if (!synHeld && !usbHeld) {
                // Both released — reset to idle
                handoff.state = HANDOFF_IDLE;
                return 0;
            }
            // User controls, synthetic ignored
            return usbHeld ? buttonMask : 0;
    }

    return 0;
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
        // Apply sensitivity reduction
        if (sensReductionAmmountX >= 0 && sensReductionAmmountX <= 100) {
            usbXLong = (usbXLong * sensReductionAmmountX);
            sensReductionXAccumulator += usbXLong % 100;
            usbXLong = usbXLong / 100;

            // Apply accumulated movement
            if (abs(sensReductionXAccumulator) >= 100) {
                usbX += sensReductionXAccumulator / 100;
                sensReductionXAccumulator %= 100;
            }
        }

        if (sensReductionAmmountY >= 0 && sensReductionAmmountY <= 100) {
            usbYLong = (usbYLong * sensReductionAmmountY);
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
        // No sensitivity reduction, just add serial movement
        usbX += serialX;
        usbY += serialY;
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
