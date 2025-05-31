// SunshineUSBProxy.ino - Using direct objects with HIDMouseDescriptorHandler
#include <USBHost_t36.h>
#include "USBHostDriver.h"
#include "HIDMouseDescriptorHandler.h"

// USB device functions
extern "C" {
    #include "usb_dev.h"
    void usb_init(void);
}

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

// Debug default mode control
bool debugMode = true;

// Mouse state
MouseState currentMouse;
uint8_t rawDataBuffer[64];
uint32_t rawDataLength = 0;

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
}

void loop() {
    // Always process USB Host tasks
    myusb.Task();
    
    // State machine
    switch (currentState) {
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
            } else {
                Serial4.println("[MAIN]: Failed to get HID descriptor - using boot protocol");
                hidMouseHandler.setBootMouseFormat();
                hidParsed = true;
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
    if (Serial4.available()) {
        char cmd = Serial4.read();
        handleSerialCommand(cmd);
    }
    
    // Status LED
    updateStatusLED();
}

void handleSerialCommand(char cmd) {
    switch (cmd) {
        case 'd':
        case 'D':
            debugMode = !debugMode;
            hidMouseHandler.setDebugOutput(debugMode);
            Serial4.print("[MAIN]: Debug mode ");
            Serial4.println(debugMode ? "ON" : "OFF");
            if (debugMode) {
                Serial4.println("[MAIN]: Move the mouse to see data...");
            }
            break;
            
        case 's':
        case 'S':
            printStatus();
            break;
            
        case 'i':
        case 'I':
            if (hidMouseHandler.isReady()) {
                hidMouseHandler.printInterfaceInfo();
                hidMouseHandler.printDescriptorInfo();
            } else {
                Serial4.println("[MAIN]: HID handler not ready");
            }
            break;
            
        case 'h':
        case 'H':
        case '?':
            printHelp();
            break;
            
        case '\r':
        case '\n':
            break;
            
        default:
            Serial4.print("[MAIN]: Unknown command: ");
            Serial4.println(cmd);
            break;
    }
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
    
    Serial4.println("====================");
}

void printHelp() {
    Serial4.println("\n=== Commands ===");
    Serial4.println("d - Toggle debug mode");
    Serial4.println("s - Show status");
    Serial4.println("i - Show interface/descriptor info");
    Serial4.println("h - Show help");
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