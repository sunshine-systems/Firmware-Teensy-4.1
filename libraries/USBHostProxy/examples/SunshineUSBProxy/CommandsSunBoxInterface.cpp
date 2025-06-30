#include "CommandsSunBoxInterface.h"
#include "SunBoxStartup.h"
#include "SunBoxLogger.h"
#include "Config.h"

// FirmwareSettings implementation
void FirmwareSettings::updateSettings(const uint8_t* data) {
    uint8_t settingId = data[0];
    int16_t settingValue = data[1] | (data[2] << 8);  // Combine two bytes into a 16-bit integer

    switch (settingId) {
        case 0:
            logger.info("V: 5");
            break;
        case 1:
            logPerformanceMetrics = settingValue;
            printSettingChange("logPerformanceMetrics", String(logPerformanceMetrics));
            break;
        case 2:
            enableSensReduction = settingValue;
            printSettingChange("enableSensReduction", String(enableSensReduction));
            break;
        case 3:
            sensReductionDurationMilliseconds = settingValue;
            printSettingChange("sensReductionDurationMilliseconds", String(sensReductionDurationMilliseconds));
            break;
        case 4:
            sensReductionAmmountX = settingValue;
            printSettingChange("sensReductionAmmountX", String(sensReductionAmmountX));
            break;
        case 5:
            sensReductionAmmountY = settingValue;
            printSettingChange("sensReductionAmmountY", String(sensReductionAmmountY));
            break;
        case 6:
            enableSpinning = settingValue;
            printSettingChange("enableSpinning", String(enableSpinning));
            break;
        case 7:
            spinAmountPerRotation = settingValue;
            printSettingChange("spinAmountPerRotation", String(spinAmountPerRotation));
            break;
        case 8:
            spinNumberOfRotations = settingValue;
            printSettingChange("spinNumberOfRotations", String(spinNumberOfRotations));
            break;
        case 9:
            spinDelayBetweenRotationsMilliseconds = settingValue;
            printSettingChange("spinDelayBetweenRotationsMilliseconds", String(spinDelayBetweenRotationsMilliseconds));
            break;
        case 10:
            spinLockoutMouseUntilCompletion = settingValue;
            printSettingChange("spinLockoutMouseUntilCompletion", String(spinLockoutMouseUntilCompletion));
            break;
        case 11:
            spinBeforeAfterMouseEvent = settingValue;
            printSettingChange("spinBeforeAfterMouseEvent", String(spinBeforeAfterMouseEvent));
            break;
        case 12:
            disablePassthroughForMMB = settingValue;
            printSettingChange("disablePassthroughForMMB", String(disablePassthroughForMMB));
            break;
        case 13:
            disablePassthroughForRMB = settingValue;
            printSettingChange("disablePassthroughForRMB", String(disablePassthroughForRMB));
            break;
        case 14:
            disablePassthroughForLMB = settingValue;
            printSettingChange("disablePassthroughForLMB", String(disablePassthroughForLMB));
            break;
        case 15:
            disablePassthroughForMB4 = settingValue;
            printSettingChange("disablePassthroughForMB4", String(disablePassthroughForMB4));
            break;
        case 16:
            disablePassthroughForMB5 = settingValue;
            printSettingChange("disablePassthroughForMB5", String(disablePassthroughForMB5));
            break;
        default:
            logger.info("I: Unknown setting ID received -> " + String(settingId));
            logger.info("I: Unknown setting ID Value received -> " + String(settingValue));
            break;
    }
}

void FirmwareSettings::printSettingChange(const String& settingName, const String& value) const {
    logger.info("I: Setting changed - " + settingName + ": " + value);
}

// CommandsSunBoxInterface implementation
CommandsSunBoxInterface::CommandsSunBoxInterface()
    : previousMouseButtons(0), mouseButtons(0), scrollWheel(0),
      xMovement(0), yMovement(0), isDataAvailable(false) {
}

void CommandsSunBoxInterface::begin() {
    // Initialize if needed
}

void CommandsSunBoxInterface::processSerial(Stream& serial) {
    static uint8_t commandBuffer[8];  // Buffer size to accommodate data (max 8 bytes)
    static uint8_t expectedLength = 0;  // Length of the message

    uint8_t dataBuffer[9];  // Buffer to hold the entire 9 bytes of data

    // Read the entire 9 bytes if available
    if (serial.available() >= 9) {
        for (uint8_t i = 0; i < 9; i++) {
            dataBuffer[i] = serial.read();
        }

        // The first byte is the length prefix
        expectedLength = dataBuffer[0];

        // Copy the relevant bytes into the commandBuffer based on the length prefix
        for (uint8_t i = 0; i < expectedLength; i++) {
            commandBuffer[i] = dataBuffer[i + 1];
        }

        if (expectedLength == 8) {
            // If the length byte is 8, it's a HID message
            processAndSetHIDReportData(commandBuffer);
        } else if (expectedLength == 3) {
            // If the length byte is 3, it's a settings message
            firmwareSettings.updateSettings(commandBuffer);
        }
    }
}

void CommandsSunBoxInterface::processLegacyData(const uint8_t* data, uint8_t length) {
    if (length == 8) {
        // HID report data
        processAndSetHIDReportData(data);
    }
}

void CommandsSunBoxInterface::processAndSetHIDReportData(const uint8_t* data) {
    bool debug_enabled = SunBoxStartup::isDebugEnabled();
    
    previousMouseButtons = mouseButtons;
    mouseButtons = data[0];
    scrollWheel = data[3];
    xMovement = parseX_00A6(data);
    yMovement = parseY_00A6(data);
    isDataAvailable = true;  // Set dataReceived to true when new data is set
    
    // Debug output
    if (debug_enabled) {
        Serial4.print("I: Mouse data - Buttons:0x");
        if (mouseButtons < 0x10) Serial4.print("0");
        Serial4.print(mouseButtons, HEX);
        Serial4.print(" X:");
        Serial4.print(xMovement);
        Serial4.print(" Y:");
        Serial4.print(yMovement);
        Serial4.print(" Wheel:");
        Serial4.println(scrollWheel);
    }
}

int16_t CommandsSunBoxInterface::parseX_00A6(const uint8_t* data) {
    int16_t x;
    uint8_t overflowByte = data[1];

    if (overflowByte == 0x80 || overflowByte == 0x7F) {
        x = twosComplement_00A6(data[5], data[4]);
    } else {
        x = static_cast<int16_t>(data[1]);
        if (x & (1 << 7)) {
            x -= 1 << 8;
        }
    }

    return x;
}

int16_t CommandsSunBoxInterface::parseY_00A6(const uint8_t* data) {
    int16_t y;
    uint8_t overflowByte = data[2];

    if (overflowByte == 0x80 || overflowByte == 0x7F) {
        y = twosComplement_00A6(data[7], data[6]);
    } else {
        y = static_cast<int16_t>(data[2]);
        if (y & (1 << 7)) {
            y -= 1 << 8;
        }
    }

    return y;
}

int16_t CommandsSunBoxInterface::twosComplement_00A6(uint8_t highByte, uint8_t lowByte) {
    int16_t value = (highByte << 8) | lowByte;
    if (value & (1 << 15)) {
        value -= 1UL << 16;
    }
    return value;
}

void CommandsSunBoxInterface::reset() {
    isDataAvailable = false;
}

MouseState CommandsSunBoxInterface::getMouseState() const {
    MouseState state;
    state.buttons = mouseButtons;
    state.x = xMovement;
    state.y = yMovement;
    state.wheel = scrollWheel;
    return state;
}