// SunshineUSBProxy.ino - Using direct objects like original code
#include <USBHost_t36.h>
#include "USBHostDriver.h"
#include "HIDReportParser.h"

// USB device functions
extern "C" {
    #include "usb_dev.h"
    void usb_init(void);
}

// Global objects - created directly, not as pointers
USBHost myusb;
USBHostDriver usbHostDriver(myusb);  // Direct object creation
HIDReportParser hidReportParser;      // Direct object creation

// State machine
enum State {
    INIT,
    WAIT_FOR_DEVICE,
    DEVICE_DETECTED,
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

// Data callback for USBHostDriver
void dataCallback(const uint8_t* data, uint32_t length) {
    // Handle incoming HID data
    if (hidParsed) {
        if (hidReportParser.parseMouseData(data, length, currentMouse)) {
            if (debugMode) {
                hidReportParser.printMouseState(currentMouse);
            }
            
            // TODO: Forward to USB device stack when ready
            if (usbDeviceInitialized) {
                // Forward the data through USB device
            }
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
    
    // Note: USBHostDriver already registered itself in its constructor
    Serial4.println("[MAIN]: USB Host Driver created (registered in constructor)");
    
    // Configure the driver
    usbHostDriver.setDataCallback(dataCallback);
    
    // Configure HID parser
    Serial4.println("[MAIN]: Configuring HID Report Parser...");
    hidReportParser.setBootMouseFormat(); // Default to boot protocol
    
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
            // Try to get and parse HID descriptor
            {
                const uint8_t* descriptor = nullptr;
                uint16_t descriptorLen = 0;
                
                Serial4.println("[MAIN]: Attempting to get HID descriptor...");
                
                if (usbHostDriver.getHIDDescriptor(&descriptor, &descriptorLen)) {
                    Serial4.println("[MAIN]: Got HID descriptor, parsing...");
                    if (hidReportParser.parseDescriptor(descriptor, descriptorLen)) {
                        Serial4.println("[MAIN]: HID descriptor parsed successfully!");
                        hidReportParser.printDescriptorInfo();
                        hidParsed = true;
                    }
                } else {
                    Serial4.println("[MAIN]: No HID descriptor available, using boot protocol");
                    hidParsed = true; // Boot protocol is already set
                }
                
                currentState = HID_PARSED;
            }
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
            
            // Check if device disconnected
            if (!usbHostDriver.isReady()) {
                Serial4.println("\n[MAIN]: Device disconnected!");
                
                // TODO: Properly shut down USB device stack
                // For now, just reset state
                deviceClaimed = false;
                hidParsed = false;
                usbDeviceInitialized = false;
                currentState = WAIT_FOR_DEVICE;
                
                // Reset parser to boot protocol
                hidReportParser.setBootMouseFormat();
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
            Serial4.print("[MAIN]: Debug mode ");
            Serial4.println(debugMode ? "ON" : "OFF");
            break;
            
        case 's':
        case 'S':
            printStatus();
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