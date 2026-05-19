#include "SunBoxCommands.h"
#include "CommandsSunBoxDevtoolsInterface.h"
#include "CommandsSunBoxInterface.h"
#include "CommandsSunBoxKMBoxInterface.h"
#include "SunBoxStartup.h"

SunBoxCommands::SunBoxCommands(Stream& serial)
    : serial(serial), bufferIndex(0), lastCharTime(0), currentMode(MODE_DETECTING) {
    
    // Create interface handlers
    devtoolsInterface = new CommandsSunBoxDevtoolsInterface();
    legacyInterface = new CommandsSunBoxInterface();
    kmboxInterface = new CommandsSunBoxKMBoxInterface();
    
    memset(routingBuffer, 0, sizeof(routingBuffer));
}

SunBoxCommands::~SunBoxCommands() {
    delete devtoolsInterface;
    delete legacyInterface;
    delete kmboxInterface;
}

void SunBoxCommands::begin() {
    // Initialize all interfaces
    devtoolsInterface->begin();
    legacyInterface->begin();
    kmboxInterface->begin();
    
    // Apply debug mode to legacy and kmbox interfaces if enabled
    if (devtoolsInterface->isDebugEnabled()) {
        Serial4.println("S: Debug mode enabled from EEPROM");
    }
}

void SunBoxCommands::check() {
    // Read available data into buffer
    while (serial.available() && bufferIndex < BUFFER_SIZE - 1) {
        routingBuffer[bufferIndex++] = serial.read();
        lastCharTime = millis();
    }
    
    // Process buffer if we have data
    if (bufferIndex > 0) {
        processBuffer();
    }
}

void SunBoxCommands::processBuffer() {
    // If we're already routing to a specific handler, continue
    if (currentMode != MODE_DETECTING) {
        switch (currentMode) {
            case MODE_DEVTOOLS:
                routeToDevtools();
                break;
            case MODE_LEGACY:
                routeToLegacy();
                break;
            case MODE_KMBOX:
                routeToKMBox();
                break;
            default:
                break;
        }
        return;
    }
    
    // Detect what type of data this is
    detectAndRoute();
}

void SunBoxCommands::detectAndRoute() {
    // Need at least one byte to detect
    if (bufferIndex == 0) return;
    
    // Check for legacy protocol (starts with 3 or 8)
    if (routingBuffer[0] == 3 || routingBuffer[0] == 8) {
        // Legacy protocol - need exactly 9 bytes
        if (bufferIndex >= 9) {
            currentMode = MODE_LEGACY;
            routeToLegacy();
        }
        // Otherwise wait for more data
        return;
    }
    
    // Check if this looks like text (printable ASCII or control chars)
    bool isText = true;
    for (size_t i = 0; i < bufferIndex; i++) {
        uint8_t c = routingBuffer[i];
        if (c != '\n' && c != '\r' && c != '\b' && c != 127 && c != '!' &&
            (c < 32 || c > 126)) {
            isText = false;
            break;
        }
    }
    
    if (!isText) {
        // Not text and not legacy protocol - clear buffer
        clearBuffer();
        return;
    }
    
    // It's text - check for line ending or timeout
    bool hasLineEnding = false;
    for (size_t i = 0; i < bufferIndex; i++) {
        if (routingBuffer[i] == '\n' || routingBuffer[i] == '\r' || routingBuffer[i] == '!') {
            hasLineEnding = true;
            break;
        }
    }
    
    // If we have a line ending or timeout, route the text
    if (hasLineEnding || (millis() - lastCharTime > 1000)) {
        // Check if it's a KMBox command
        if (isKMBoxCommand(routingBuffer, bufferIndex)) {
            currentMode = MODE_KMBOX;
            routeToKMBox();
        } else {
            // Default to devtools for other text commands
            currentMode = MODE_DEVTOOLS;
            routeToDevtools();
        }
    }
}

void SunBoxCommands::routeToDevtools() {
    // Convert buffer to string and send to devtools
    String command = "";
    
    for (size_t i = 0; i < bufferIndex; i++) {
        char c = (char)routingBuffer[i];
        
        // Handle line endings
        if (c == '\n' || c == '\r' || c == '!') {
            if (command.length() > 0) {
                devtoolsInterface->handleCommand(command);
                command = "";
            }
        } else if (c == '\b' || c == 127) {  // Backspace
            if (command.length() > 0) {
                command.remove(command.length() - 1);
            }
        } else if (c >= ' ' && c <= '~') {  // Printable characters
            command += c;
        }
    }
    
    // Handle remaining command after timeout
    if (command.length() > 0 && (millis() - lastCharTime > 1000)) {
        devtoolsInterface->handleCommand(command);
    }
    
    // Clear buffer and reset mode
    clearBuffer();
}

void SunBoxCommands::routeToLegacy() {
    // Legacy protocol expects exactly 9 bytes
    if (bufferIndex >= 9) {
        // Create a mock stream for the legacy interface
        // Since we can't easily create a stream, we'll modify the legacy interface
        // For now, we'll process it directly here
        
        uint8_t dataBuffer[9];
        memcpy(dataBuffer, routingBuffer, 9);
        
        uint8_t expectedLength = dataBuffer[0];
        uint8_t commandBuffer[8];
        
        // Copy the relevant bytes
        for (uint8_t i = 0; i < expectedLength && i < 8; i++) {
            commandBuffer[i] = dataBuffer[i + 1];
        }
        
        // Process based on length
        if (expectedLength == 8) {
            // HID message - process directly
            legacyInterface->processLegacyData(commandBuffer, expectedLength);
        } else if (expectedLength == 3) {
            // Settings message
            legacyInterface->getSettings().updateSettings(commandBuffer);
        }
        
        // Remove processed bytes from buffer
        if (bufferIndex > 9) {
            memmove(routingBuffer, routingBuffer + 9, bufferIndex - 9);
            bufferIndex -= 9;
        } else {
            clearBuffer();
        }
    }
}

void SunBoxCommands::routeToKMBox() {
    // Convert buffer to string and send to KMBox
    String command = "";
    size_t processedBytes = 0;
    
    for (size_t i = 0; i < bufferIndex; i++) {
        char c = (char)routingBuffer[i];
        
        // Handle line endings
        if (c == '\n' || c == '\r' || c == ';') {
            processedBytes = i + 1;
            if (command.length() > 0) {
                kmboxInterface->processCommand(command);
                command = "";
            }
            break;
        } else if (c >= 32 && c <= 126) {  // Printable ASCII
            command += c;
        }
    }
    
    // Handle timeout
    if (processedBytes == 0 && (millis() - lastCharTime > 100)) {
        if (command.length() > 0) {
            kmboxInterface->processCommand(command);
        }
        processedBytes = bufferIndex;
    }
    
    // Remove processed bytes
    if (processedBytes > 0) {
        if (bufferIndex > processedBytes) {
            memmove(routingBuffer, routingBuffer + processedBytes, bufferIndex - processedBytes);
            bufferIndex -= processedBytes;
        } else {
            clearBuffer();
        }
    }
}

bool SunBoxCommands::isKMBoxCommand(const uint8_t* data, size_t len) {
    // Check if it starts with "km."
    if (len >= 3) {
        return (data[0] == 'k' && data[1] == 'm' && data[2] == '.');
    }
    return false;
}

void SunBoxCommands::clearBuffer() {
    bufferIndex = 0;
    currentMode = MODE_DETECTING;
    memset(routingBuffer, 0, sizeof(routingBuffer));
}

bool SunBoxCommands::hasData() const {
    return legacyInterface->hasData() || kmboxInterface->hasData();
}

MouseState SunBoxCommands::getMouseState() const {
    // Priority: Legacy > KMBox
    if (legacyInterface->hasData()) {
        return legacyInterface->getMouseState();
    } else if (kmboxInterface->hasData()) {
        return kmboxInterface->getMouseState();
    }
    
    MouseState empty;
    empty.clear();
    return empty;
}

void SunBoxCommands::resetData() {
    legacyInterface->reset();
    kmboxInterface->reset();
}