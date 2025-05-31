// SunshineUSBProxy.ino - Using direct objects with HIDMouseDescriptorHandler
#include <USBHost_t36.h>
#include <EEPROM.h>
#include "USBHostDriver.h"
#include "HIDMouseDescriptorHandler.h"

// USB device functions
extern "C" {
    #include "usb_dev.h"
    void usb_init(void);
}

// EEPROM structure for forced interface selection
struct ForceClaimConfig {
    uint32_t magic;      // 0xDEADBEEF for valid data
    uint16_t vid;
    uint16_t pid;
    uint8_t interface_num;
    uint8_t endpoint_addr;
    uint16_t endpoint_size;
};

#define EEPROM_MAGIC 0xDEADBEEF
#define EEPROM_CONFIG_ADDR 0

// Global objects - created directly, not as pointers
USBHost myusb;
USBHostDriver usbHostDriver(myusb);         // Direct object creation
HIDMouseDescriptorHandler hidMouseHandler;   // Direct object creation

// State machine
enum State {
    INIT,
    WAIT_FOR_DEVICE,
    DEVICE_DETECTED,
    HID_DESCRIPTOR_WAIT,
    HID_PARSED,
    USB_DEVICE_READY,
    PROXY_ACTIVE
};

State currentState = INIT;

// Flags
bool deviceClaimed = false;
bool hidParsed = false;
bool usbDeviceInitialized = false;

// Debug control
bool debugMode = false;

// Mouse state
MouseState currentMouse;
uint8_t rawDataBuffer[64];
uint32_t rawDataLength = 0;

// Command buffer
String commandBuffer = "";
unsigned long lastCommandChar = 0;

// Data callback for USBHostDriver
void dataCallback(const uint8_t* data, uint32_t length) {
    // Diagnostic: print if we're getting zero-length callbacks
    static uint32_t callback_count = 0;
    callback_count++;
    
    if (debugMode && length == 0) {
        Serial4.print("[DATA]: Empty callback #");
        Serial4.println(callback_count);
    }
    
    // Store raw data for potential use
    if (length > 0 && length <= sizeof(rawDataBuffer)) {
        memcpy(rawDataBuffer, data, length);
        rawDataLength = length;
    }
    
    // Debug raw data if enabled
    if (debugMode && length > 0) {
        Serial4.print("[DATA]: Raw HID data (");
        Serial4.print(length);
        Serial4.print(" bytes): ");
        for (uint32_t i = 0; i < length; i++) {
            if (data[i] < 0x10) Serial4.print("0");
            Serial4.print(data[i], HEX);
            if (i < length - 1) Serial4.print(" ");
        }
        Serial4.println();
        
        // Additional debug - show bits for first byte
        if (length > 0) {
            Serial4.print("[DATA]: Button byte bits: ");
            for (int i = 7; i >= 0; i--) {
                Serial4.print((data[0] >> i) & 1);
            }
            Serial4.print(" (0x");
            if (data[0] < 0x10) Serial4.print("0");
            Serial4.print(data[0], HEX);
            Serial4.println(")");
        }
    }
    
    // Handle incoming HID data
    if (hidParsed && hidMouseHandler.isReady()) {
        if (hidMouseHandler.parseMouseData(data, length, currentMouse)) {
            if (debugMode) {
                Serial4.print("[DATA]: Parsed - ");
                hidMouseHandler.printMouseState(currentMouse);
            }
            
            // TODO: Forward to USB device stack when ready
            if (usbDeviceInitialized) {
                // Forward the data through USB device
            }
        } else if (debugMode) {
            Serial4.println("[DATA]: Failed to parse mouse data");
        }
    }
}

void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
    
    // Wait for Serial4 to be ready (initialized by startup.c)
    delay(100);
    
    Serial4.println("\n=======================================");
    Serial4.println("  SunBox USB Proxy Starting");
    Serial4.println("=======================================");
    
    // Initialize HID handler with the USB driver
    hidMouseHandler.begin(&usbHostDriver);
    hidMouseHandler.setDebugOutput(true); // Enable debug output
    
    // Note: USBHostDriver already registered itself in its constructor
    Serial4.println("[MAIN]: USB Host Driver created (registered in constructor)");
    
    // Configure the driver
    usbHostDriver.setDataCallback(dataCallback);
    
    // Start USB Host
    Serial4.println("[MAIN]: Starting USB Host...");
    myusb.begin();
    
    // Initialize driver (compatibility call)
    if (!usbHostDriver.begin()) {
        Serial4.println("[MAIN]: ERROR - Failed to initialize USB Host Driver!");
        while(1); // Halt
    }
    
    Serial4.println("[MAIN]: Setup complete - waiting for device...");
    currentState = WAIT_FOR_DEVICE;
    lastCommandChar = millis(); // Initialize timer
}

void loop() {
    // Always process USB Host tasks
    myusb.Task();
    
    // State machine
    switch (currentState) {
        case INIT:
            // Should not reach here after setup()
            break;
            
        case WAIT_FOR_DEVICE:
            if (usbHostDriver.isReady()) {
                Serial4.println("\n[MAIN]: Device detected and claimed!");
                Serial4.print("[MAIN]: VID: 0x");
                Serial4.print(usbHostDriver.getVendorID(), HEX);
                Serial4.print(", PID: 0x");
                Serial4.println(usbHostDriver.getProductID(), HEX);
                
                deviceClaimed = true;
                currentState = DEVICE_DETECTED;
            }
            break;
            
        case DEVICE_DETECTED:
            // Setup mouse interface
            Serial4.println("[MAIN]: Setting up HID mouse interface...");
            
            if (hidMouseHandler.setupMouseInterface()) {
                Serial4.println("[MAIN]: Mouse interface found, requesting descriptor...");
                currentState = HID_DESCRIPTOR_WAIT;
            } else {
                Serial4.println("[MAIN]: No HID mouse interface found!");
                // For now, continue anyway - might be a non-standard device
                currentState = HID_DESCRIPTOR_WAIT;
            }
            break;
            
        case HID_DESCRIPTOR_WAIT:
            // Request and parse HID descriptor
            Serial4.println("[MAIN]: Requesting HID descriptor...");
            
            if (hidMouseHandler.requestHIDDescriptor(1000)) {  // 1 second timeout
                Serial4.println("[MAIN]: HID descriptor processed!");
                
                if (hidMouseHandler.isReady()) {
                    Serial4.println("[MAIN]: HID parser is ready!");
                    hidMouseHandler.printDescriptorInfo();
                    hidParsed = true;
                } else {
                    Serial4.println("[MAIN]: HID parser not ready - using boot protocol");
                    hidMouseHandler.setBootMouseFormat();
                    hidParsed = true;
                }
                
                // Activate the HID interface (simplified - no longer sends optional HID commands)
                Serial4.println("[MAIN]: Activating HID interface...");
                hidMouseHandler.activateInterface();
                Serial4.println("[MAIN]: HID interface activated");
                
            } else {
                Serial4.println("[MAIN]: Failed to get HID descriptor - using boot protocol");
                hidMouseHandler.setBootMouseFormat();
                hidParsed = true;
                
                // Still try to activate even with boot protocol
                Serial4.println("[MAIN]: Activating HID interface...");
                hidMouseHandler.activateInterface();
                Serial4.println("[MAIN]: HID interface activated");
            }
            
            currentState = HID_PARSED;
            break;
            
        case HID_PARSED:
            // NOW we can safely initialize USB device stack
            Serial4.println("\n[MAIN]: HID device ready - initializing USB device stack...");
            
            // TODO: Configure USB device based on detected device
            // For now, just initialize
            usb_init();
            
            usbDeviceInitialized = true;
            currentState = USB_DEVICE_READY;
            
            Serial4.println("[MAIN]: USB device stack initialized!");
            break;
            
        case USB_DEVICE_READY:
            // Everything is ready - transition to active proxy mode
            Serial4.println("[MAIN]: Proxy fully initialized and ready!");
            currentState = PROXY_ACTIVE;
            
            // Flash LED to indicate ready
            for (int i = 0; i < 5; i++) {
                digitalWrite(LED_BUILTIN, HIGH);
                delay(100);
                digitalWrite(LED_BUILTIN, LOW);
                delay(100);
            }
            break;
            
        case PROXY_ACTIVE:
            // Normal operation - data is forwarded via callbacks
            
            // Diagnostic: periodically check if we're getting data
            static unsigned long lastDataCheck = 0;
            if (debugMode && millis() - lastDataCheck > 5000) {
                lastDataCheck = millis();
                Serial4.println("[MAIN]: Still in PROXY_ACTIVE, checking data flow...");
                
                // Try to manually check for data
                uint8_t testBuffer[64];
                uint32_t testLength = 0;
                if (usbHostDriver.getLastData(testBuffer, testLength)) {
                    Serial4.print("[MAIN]: Found pending data! Length: ");
                    Serial4.println(testLength);
                }
            }
            
            // Check if device disconnected
            if (!usbHostDriver.isReady()) {
                Serial4.println("\n[MAIN]: Device disconnected!");
                
                // TODO: Properly shut down USB device stack
                // For now, just reset state
                deviceClaimed = false;
                hidParsed = false;
                usbDeviceInitialized = false;
                currentState = WAIT_FOR_DEVICE;
            }
            break;
    }
    
    // Handle Serial commands
    while (Serial4.available()) {
        char c = Serial4.read();
        handleSerialCharacter(c);
        lastCommandChar = millis();
    }
    
    // Execute command after 1 second of no input (fallback for terminals without line endings)
    if (commandBuffer.length() > 0 && (millis() - lastCommandChar > 1000)) {
        Serial4.println(); // New line
        handleSerialCommand(commandBuffer);
        commandBuffer = "";
    }
    
    // Status LED
    updateStatusLED();
}

void handleSerialCharacter(char c) {
    // Simple approach - process on Enter or specific trigger
    if (c == '\n' || c == '\r' || c == '!') {  // Added '!' as manual trigger
        if (commandBuffer.length() > 0) {
            Serial4.println(); // New line
            handleSerialCommand(commandBuffer);
            commandBuffer = "";
        }
    } else if (c == '\b' || c == 127) { // Backspace
        if (commandBuffer.length() > 0) {
            commandBuffer.remove(commandBuffer.length() - 1);
            Serial4.print("\b \b"); // Erase character on screen
        }
    } else if (c >= ' ' && c <= '~') { // Printable characters
        Serial4.print(c); // Echo
        commandBuffer += c;
    }
}

void handleSerialCommand(String cmd) {
    cmd.trim();
    if (cmd.length() == 0) return;
    
    // Parse command and arguments
    int spaceIndex = cmd.indexOf(' ');
    String command = (spaceIndex > 0) ? cmd.substring(0, spaceIndex) : cmd;
    String args = (spaceIndex > 0) ? cmd.substring(spaceIndex + 1) : "";
    
    // Process commands - check longer commands first
    if (command == "dump") {
        Serial4.println("[MAIN]: Device descriptor dump:");
        usbHostDriver.dumpDeviceInfo();
    }
    else if (command == "debug") {
        debugMode = !debugMode;
        hidMouseHandler.setDebugOutput(debugMode);
        Serial4.print("[MAIN]: Debug mode ");
        Serial4.println(debugMode ? "ON" : "OFF");
        if (debugMode) {
            Serial4.println("[MAIN]: Move the mouse to see data...");
        }
    }
    else if (command == "force") {
        handleForceClaimCommand(args);
    }
    else if (command == "clear") {
        clearForceClaim();
    }
    else if (command == "status") {
        printStatus();
    }
    else if (command == "info") {
        if (hidMouseHandler.isReady()) {
            hidMouseHandler.printInterfaceInfo();
            hidMouseHandler.printDescriptorInfo();
        } else {
            Serial4.println("[MAIN]: HID handler not ready");
        }
    }
    else if (command == "help" || command == "?") {
        printHelp();
    }
    else {
        Serial4.print("[MAIN]: Unknown command: ");
        Serial4.println(command);
        Serial4.println("[MAIN]: Type 'help' for available commands");
    }
}

void handleForceClaimCommand(String args) {
    // Parse fc arguments: vid,pid,interface,endpoint
    // Example: fc 046d,c53f,1,82
    
    int commas[3];
    int commaCount = 0;
    
    // Find comma positions
    for (unsigned int i = 0; i < args.length() && commaCount < 3; i++) {
        if (args.charAt(i) == ',') {
            commas[commaCount++] = i;
        }
    }
    
    if (commaCount != 3) {
        Serial4.println("[MAIN]: Invalid format! Use: force vid,pid,interface,endpoint");
        Serial4.println("[MAIN]: Example: force 046d,c53f,1,82");
        return;
    }
    
    // Parse values
    String vidStr = args.substring(0, commas[0]);
    String pidStr = args.substring(commas[0] + 1, commas[1]);
    String ifaceStr = args.substring(commas[1] + 1, commas[2]);
    String epStr = args.substring(commas[2] + 1);
    
    // Convert to numbers
    uint16_t vid = strtoul(vidStr.c_str(), NULL, 16);
    uint16_t pid = strtoul(pidStr.c_str(), NULL, 16);
    uint8_t iface = strtoul(ifaceStr.c_str(), NULL, 10);
    uint8_t ep = strtoul(epStr.c_str(), NULL, 16);
    
    // Create config
    ForceClaimConfig config;
    config.magic = EEPROM_MAGIC;
    config.vid = vid;
    config.pid = pid;
    config.interface_num = iface;
    config.endpoint_addr = ep;
    config.endpoint_size = 64; // Default, will be updated from descriptor
    
    // Write to EEPROM
    EEPROM.put(EEPROM_CONFIG_ADDR, config);
    
    Serial4.println("[MAIN]: Force claim configuration saved:");
    Serial4.print("[MAIN]: VID=0x");
    Serial4.print(vid, HEX);
    Serial4.print(" PID=0x");
    Serial4.print(pid, HEX);
    Serial4.print(" Interface=");
    Serial4.print(iface);
    Serial4.print(" Endpoint=0x");
    Serial4.println(ep, HEX);
    Serial4.println("[MAIN]: Configuration will be used on next device connection");
}

void clearForceClaim() {
    ForceClaimConfig config;
    config.magic = 0; // Invalid magic number
    EEPROM.put(EEPROM_CONFIG_ADDR, config);
    Serial4.println("[MAIN]: Force claim configuration cleared");
}

void printStatus() {
    Serial4.println("\n=== System Status ===");
    Serial4.print("State: ");
    switch (currentState) {
        case INIT: Serial4.println("INIT"); break;
        case WAIT_FOR_DEVICE: Serial4.println("WAIT_FOR_DEVICE"); break;
        case DEVICE_DETECTED: Serial4.println("DEVICE_DETECTED"); break;
        case HID_DESCRIPTOR_WAIT: Serial4.println("HID_DESCRIPTOR_WAIT"); break;
        case HID_PARSED: Serial4.println("HID_PARSED"); break;
        case USB_DEVICE_READY: Serial4.println("USB_DEVICE_READY"); break;
        case PROXY_ACTIVE: Serial4.println("PROXY_ACTIVE"); break;
    }
    
    Serial4.print("Device: ");
    if (usbHostDriver.isReady()) {
        Serial4.print("Connected (VID:0x");
        Serial4.print(usbHostDriver.getVendorID(), HEX);
        Serial4.print(" PID:0x");
        Serial4.print(usbHostDriver.getProductID(), HEX);
        Serial4.println(")");
    } else {
        Serial4.println("Not connected");
    }
    
    Serial4.print("HID Handler: ");
    if (hidMouseHandler.isReady()) {
        Serial4.print("Ready (Interface ");
        Serial4.print(hidMouseHandler.getInterfaceNumber());
        Serial4.print(", EP 0x");
        Serial4.print(hidMouseHandler.getEndpointAddress() | 0x80, HEX);
        Serial4.println(")");
    } else {
        Serial4.print("Not ready (State: ");
        switch (hidMouseHandler.getState()) {
            case HID_STATE_IDLE: Serial4.print("IDLE"); break;
            case HID_STATE_WAIT_DESCRIPTOR: Serial4.print("WAIT_DESCRIPTOR"); break;
            case HID_STATE_PARSING: Serial4.print("PARSING"); break;
            case HID_STATE_READY: Serial4.print("READY"); break;
            case HID_STATE_ERROR: Serial4.print("ERROR"); break;
        }
        Serial4.println(")");
    }
    
    Serial4.print("HID Parser: ");
    Serial4.println(hidParsed ? "Parsed" : "Not parsed");
    
    Serial4.print("USB Device Stack: ");
    Serial4.println(usbDeviceInitialized ? "Initialized" : "Not initialized");
    
    Serial4.print("Debug Mode: ");
    Serial4.println(debugMode ? "ON" : "OFF");
    
    // Check for force claim config
    ForceClaimConfig config;
    EEPROM.get(EEPROM_CONFIG_ADDR, config);
    if (config.magic == EEPROM_MAGIC) {
        Serial4.print("Force Claim: VID=0x");
        Serial4.print(config.vid, HEX);
        Serial4.print(" PID=0x");
        Serial4.print(config.pid, HEX);
        Serial4.print(" Interface=");
        Serial4.print(config.interface_num);
        Serial4.print(" Endpoint=0x");
        Serial4.println(config.endpoint_addr, HEX);
    } else {
        Serial4.println("Force Claim: Not configured");
    }
    
    Serial4.println("====================");
}

void printHelp() {
    Serial4.println("\n=== Commands ===");
    Serial4.println("debug  - Toggle debug mode");
    Serial4.println("dump   - Dump device descriptors");
    Serial4.println("force vid,pid,interface,endpoint - Force claim specific interface");
    Serial4.println("         Example: force 046d,c53f,1,82");
    Serial4.println("clear  - Clear force claim configuration");
    Serial4.println("status - Show system status");
    Serial4.println("info   - Show interface/descriptor info");
    Serial4.println("help   - Show this help");
    Serial4.println("");
    Serial4.println("Note: Commands execute on Enter, '!' key, or after 1 second");
    Serial4.println("If using Arduino Serial Monitor, set to 'Newline' or 'Carriage return'");
    Serial4.println("================");
}

void updateStatusLED() {
    static unsigned long lastBlink = 0;
    unsigned long now = millis();
    
    switch (currentState) {
        case WAIT_FOR_DEVICE:
            // Slow blink
            if (now - lastBlink >= 1000) {
                digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
                lastBlink = now;
            }
            break;
            
        case DEVICE_DETECTED:
        case HID_DESCRIPTOR_WAIT:
        case HID_PARSED:
        case USB_DEVICE_READY:
            // Fast blink
            if (now - lastBlink >= 200) {
                digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
                lastBlink = now;
            }
            break;
            
        case PROXY_ACTIVE:
            // Solid on
            digitalWrite(LED_BUILTIN, HIGH);
            break;
            
        default:
            // Off
            digitalWrite(LED_BUILTIN, LOW);
            break;
    }
}