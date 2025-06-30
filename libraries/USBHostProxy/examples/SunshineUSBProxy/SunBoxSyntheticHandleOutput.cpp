#include "SunBoxSyntheticHandleOutput.h"
#include "SunBoxCommands.h"
#include "SunBoxUSBMouseDataHandler.h"
#include "SunBoxLogger.h"
#include "Config.h"

SunBoxSyntheticHandleOutput::SunBoxSyntheticHandleOutput(SunBoxCommands& commands, 
                                                       SunBoxUSBMouseDataHandler& usbHandler)
    : commands(commands), usbHandler(usbHandler), usbDeviceProxy(nullptr),
      mouseEndpoint(0), previousUsbButtons(0), previousSerialButtons(0),
      activationTimestamp4MouseButtonExclusion(0), activationTimestamp4MouseMovementLockout(0),
      lastRMBPressTime(0), lastLMBPressTime(0), lastMB4PressTime(0), lastMB5PressTime(0),
      sensReductionXAccumulator(0), sensReductionYAccumulator(0),
      spinActive(false), spinRotationsRemaining(0), spinNextMoveTime(0), spinCurrentX(0) {
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
        // Save previous state BEFORE getting new state
        previousSerialButtons = previousSerialState.buttons;
        
        serialState = commands.getMouseState();
        
        // Check for sensitivity reduction trigger (scroll wheel = 1)
        if (serialState.wheel == 1) {
            activationTimestamp4MouseMovementLockout = millis() + sensReductionDurationMilliseconds;
        }
        
        // Update previous state for next cycle
        previousSerialState = serialState;
        commands.resetData();
    } else {
        serialState.clear();
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
    
    // Apply button filtering
    performButtonFiltering(usbState.buttons, previousUsbButtons, unmodifiedUsbButtons);
    
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
            // Special handling for LMB/RMB
            // If USB mouse is still holding the button, ignore serial release
            if ((usbState.buttons & buttonMask) && !(serialState.buttons & buttonMask) && (previousSerialButtons & buttonMask)) {
                finalButtons |= buttonMask;
                continue;
            }
            // If both indicate release, release
            if (!(usbState.buttons & buttonMask) && !(serialState.buttons & buttonMask)) {
                // Button is released
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
    
    // WORKAROUND: The formatMouseData function incorrectly handles button bitfields
    // It treats each button as an individual 1-bit field rather than a combined 8-bit field
    // This causes any non-zero button value to be converted to 0x01 (left click)
    // Override the first byte with our correct button state
    if (outputLength > 0) {
        outputBuffer[0] = finalState.buttons;
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