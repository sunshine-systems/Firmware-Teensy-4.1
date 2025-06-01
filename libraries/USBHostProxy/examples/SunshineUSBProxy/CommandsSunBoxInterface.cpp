#include "CommandsSunBoxInterface.h"
#include "SunBoxStartup.h"

// FirmwareSettings implementation
void FirmwareSettings::updateSettings(const uint8_t* data) {
    sensitivity = data[0];
    acceleration = data[1];
    smoothing = data[2];
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
            Serial4.println("S: Settings updated");
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
    // TODO: Implement based on actual protocol
    // This is a placeholder - implement based on your actual data format
    // Assuming bytes 1-2 are X movement (little-endian)
    return (int16_t)(data[1] | (data[2] << 8));
}

int16_t CommandsSunBoxInterface::parseY_00A6(const uint8_t* data) {
    // TODO: Implement based on actual protocol
    // This is a placeholder - implement based on your actual data format
    // Assuming bytes 4-5 are Y movement (little-endian)
    return (int16_t)(data[4] | (data[5] << 8));
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