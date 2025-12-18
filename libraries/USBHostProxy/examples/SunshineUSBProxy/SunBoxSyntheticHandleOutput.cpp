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
      cpsEnabled(false), cpsClickState(false), cpsNextActionTime(0), cpsThumbPressedInWindow(false) {
    previousUsbState.clear();
    previousSerialState.clear();
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

    // Handle CPS toggle (MMB + MB5 combo or MMB alone)
    handleCPSToggle(unmodifiedUsbButtons, previousUsbButtons);

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

    // Apply CPS auto-clicking if enabled
    updateCPS(finalButtons, unmodifiedUsbButtons);

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

void SunBoxSyntheticHandleOutput::handleCPSToggle(uint8_t currentButtons, uint8_t previousButtons) {
    bool mmbJustPressed = (currentButtons & MOUSE_MIDDLE) && !(previousButtons & MOUSE_MIDDLE);

    // MB5 (back thumb) press within exclusion window = ENABLE CPS
    bool thumbBtnJustPressed = (currentButtons & MOUSE_BUTTON5) && !(previousButtons & MOUSE_BUTTON5);
    bool inExclusionWindow = (millis() - activationTimestamp4MouseButtonExclusion) <= BUTTON_EXCLUSION_DURATION_MS;

    if (thumbBtnJustPressed && inExclusionWindow) {
        cpsEnabled = true;
        cpsClickState = false;
        cpsNextActionTime = millis();
        logger.info("CPS: ENABLED (MB5 pressed in window)");
    }

    // MMB press = DISABLE CPS (also resets window for next enable)
    if (mmbJustPressed) {
        if (cpsEnabled) {
            cpsEnabled = false;
            cpsClickState = false;
            logger.info("CPS: DISABLED (MMB pressed)");
        } else {
            logger.info("CPS: MMB pressed - window started");
        }
    }
}

unsigned long SunBoxSyntheticHandleOutput::calculateNextActionDelay() {
    // For 10 CPS, we need 10 complete cycles per second
    // Each cycle = press + release = 2 actions
    // So base interval per action = 1000ms / CPS_RATE / 2 = 50ms for 10 CPS
    unsigned long baseInterval = 1000 / CPS_RATE / 2;

    // Random variance between 7-15%
    int variancePercent = random(CPS_VARIANCE_MIN_PERCENT, CPS_VARIANCE_MAX_PERCENT + 1);
    int varianceMs = (baseInterval * variancePercent) / 100;

    // Apply variance (randomly add or subtract)
    if (random(2) == 0) {
        return baseInterval + varianceMs;
    } else {
        return baseInterval - varianceMs;
    }
}

void SunBoxSyntheticHandleOutput::updateCPS(uint8_t& finalButtons, uint8_t realButtons) {
    if (!cpsEnabled) return;

    // Check if real LMB is held
    bool realLMBHeld = (realButtons & MOUSE_LEFT);

    if (realLMBHeld) {
        // Clear real LMB - we're replacing it with synthetic clicks
        finalButtons &= ~MOUSE_LEFT;

        // Check if it's time for next action
        if (millis() >= cpsNextActionTime) {
            // Toggle click state
            cpsClickState = !cpsClickState;

            // Schedule next action with randomized timing
            cpsNextActionTime = millis() + calculateNextActionDelay();
        }

        // Apply current synthetic state
        if (cpsClickState) {
            finalButtons |= MOUSE_LEFT;
        }
        // else: LMB stays cleared (release state)

    } else {
        // Real LMB released - ensure clean exit
        if (cpsClickState) {
            // We were mid-press - ensure LMB is NOT set (clean release)
            finalButtons &= ~MOUSE_LEFT;
        }
        // Reset for next activation
        cpsClickState = false;
    }
}