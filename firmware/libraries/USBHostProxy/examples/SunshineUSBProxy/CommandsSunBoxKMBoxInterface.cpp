#include "CommandsSunBoxKMBoxInterface.h"
#include "SunBoxStartup.h"

CommandsSunBoxKMBoxInterface::CommandsSunBoxKMBoxInterface()
    : commandBuffer(""), lastCharTime(0), dataAvailable(false) {
    currentState.clear();
}

void CommandsSunBoxKMBoxInterface::begin() {
    // Initialize if needed
}

void CommandsSunBoxKMBoxInterface::processSerial(Stream& serial) {
    while (serial.available()) {
        char c = serial.read();
        lastCharTime = millis();
        
        // Handle line endings
        if (c == '\n' || c == '\r' || c == ';') {
            if (commandBuffer.length() > 0) {
                processCommand(commandBuffer);
                commandBuffer = "";
            }
        } else if (c >= 32 && c <= 126) {  // Printable ASCII
            commandBuffer += c;
            
            // Prevent buffer overflow
            if (commandBuffer.length() > 100) {
                commandBuffer = "";
            }
        }
    }
    
    // Process command after timeout (100ms)
    if (commandBuffer.length() > 0 && (millis() - lastCharTime > 100)) {
        processCommand(commandBuffer);
        commandBuffer = "";
    }
}

void CommandsSunBoxKMBoxInterface::processCommand(const String& command) {
    bool debug_enabled = SunBoxStartup::isDebugEnabled();
    
    // For now, just print what we received
    if (debug_enabled) {
        Serial4.print("I: Received command: ");
        Serial4.println(command);
    }
    
    // TODO: Implement actual KMBox B+ command parsing
    // Examples:
    // km.move(x,y,wheel)
    // km.click(button)
    // km.press(button)
    // km.release(button)
    // etc.
    
    // For demonstration, mark that we have data
    dataAvailable = true;
}

void CommandsSunBoxKMBoxInterface::reset() {
    dataAvailable = false;
    // Keep current state for now
}

MouseState CommandsSunBoxKMBoxInterface::getMouseState() const {
    return currentState;
}