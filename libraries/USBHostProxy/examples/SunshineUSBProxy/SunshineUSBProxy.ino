// SunshineUSBProxy.ino - Clean Architecture USB Proxy
#include <USBHost_t36.h>
#include "USBHostDriver.h"
#include "HIDMouseDescriptorHandler.h"
#include "SunBoxCommands.h"
#include "SunBoxUSBMouseDataHandler.h"
#include "SunBoxSyntheticHandleOutput.h"
#include "CommandsSunBoxDevtoolsInterface.h"
#include "SunBoxEEPROM.h"
#include "USBDeviceProxy.h"  // NEW: Add our custom USB device stack

// Remove the USB device functions - we don't need Teensy's stack anymore
// extern "C" {
//     #include "usb_dev.h"
//     void usb_init(void);
// }

// =============================================================================
// Configuration
// =============================================================================

#define SERIAL_PORT     Serial4   // Single serial port for all commands/data

// =============================================================================
// Global Objects
// =============================================================================

// USB Host components
USBHost myusb;
USBHostDriver usbHostDriver(myusb);
HIDMouseDescriptorHandler hidMouseHandler;

// Command handler (single serial port)
SunBoxCommands sunboxCommands(SERIAL_PORT);

// USB data handler
SunBoxUSBMouseDataHandler usbMouseHandler(usbHostDriver, hidMouseHandler);

// Output handler
SunBoxSyntheticHandleOutput syntheticOutput(sunboxCommands, usbMouseHandler);

// USB Device Proxy - NEW: Our custom USB device stack
USBDeviceProxy usbDeviceProxy;

// Make USBHostDriver accessible to usb_host_wrapper
extern "C" {
    extern USBHostDriver* g_usbHostDriver;
}

// State tracking
bool systemInitialized = false;

// =============================================================================
// Setup
// =============================================================================

void setup() {
    // Initialize LED
    pinMode(LED_BUILTIN, OUTPUT);
    
    // Wait for Serial4 to be ready (initialized by startup.c)
    delay(100);
    
    // Print startup banner
    printBanner();
    
    // Set the global pointer for usb_host_wrapper
    g_usbHostDriver = &usbHostDriver;
    
    // Initialize EEPROM
    sunboxEEPROM.begin();
    
    // Initialize components
    sunboxCommands.begin();
    usbMouseHandler.begin();
    syntheticOutput.begin();
    
    // Set up component references for devtools
    CommandsSunBoxDevtoolsInterface* devtools = sunboxCommands.getDevtools();
    if (devtools) {
        devtools->setUSBHostDriver(&usbHostDriver);
        devtools->setHIDHandler(&hidMouseHandler);
        
        // Apply debug mode to HID handler if enabled
        if (devtools->isDebugEnabled()) {
            hidMouseHandler.setDebugOutput(true);
        }
    }
    
    // Initialize USB Device Proxy - NEW: This replaces Teensy's usb_init()
    Serial4.println("S: Initializing USB Device Proxy...");
    usbDeviceProxy.begin();
    
    // Start USB Host
    Serial4.println("S: Starting USB Host...");
    myusb.begin();
    
    // Initialize USB driver
    if (!usbHostDriver.begin()) {
        Serial4.println("E: Failed to initialize USB Host Driver!");
        while(1) {
            digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
            delay(100);
        }
    }
    
    systemInitialized = true;
    Serial4.println("S: System initialized - type 'help' for commands");
}

// =============================================================================
// Main Loop
// =============================================================================

void loop() {
    // Process USB Host tasks
    myusb.Task();
    
    // Poll USB Device proxy - CRITICAL: Must be called as often as possible!
    usbDeviceProxy.poll();
    
    // Check for commands
    sunboxCommands.check();
    
    // Check for USB data
    usbMouseHandler.check();
    
    // Process output if any data is available
    if (sunboxCommands.hasData() || usbMouseHandler.hasData()) {
        syntheticOutput.process();
    }
    
    // Update status based on USB device proxy state
    updateUSBDeviceStatus();
    
    // Update status LED
    updateStatusLED();
}

// =============================================================================
// Helper Functions
// =============================================================================

void printBanner() {
    Serial4.println("\n\n");
    Serial4.println("I: =======================================");
    Serial4.println("I:        SunBox USB Proxy v3.0");
    Serial4.println("I: =======================================");
    Serial4.println("I: Clean Architecture Implementation");
    Serial4.println("I: Features:");
    Serial4.println("I:   - USB HID Mouse Proxy");
    Serial4.println("I:   - Sunshine Legacy Protocol");
    Serial4.println("I:   - KMBox B+ Protocol");
    Serial4.println("I:   - Intelligent Command Routing");
    Serial4.println("I:   - Custom USB Device Stack (Polling)");  // NEW
    Serial4.println("I: =======================================\n");
}

// NEW: Replace initializeUSBDevice() with status monitoring
void updateUSBDeviceStatus() {
    static unsigned long lastStatusTime = 0;
    static bool lastConnected = false;
    
    // Only check every second
    if (millis() - lastStatusTime < 1000) return;
    lastStatusTime = millis();
    
    bool currentlyConnected = usbDeviceProxy.isConnected();
    
    // Log state changes
    if (currentlyConnected != lastConnected) {
        Serial4.print("S: USB Device State: ");
        Serial4.println(usbDeviceProxy.getStateString());
        lastConnected = currentlyConnected;
    }
    
    // Log polling statistics periodically (every 10 seconds)
    static unsigned long lastStatsTime = 0;
    if (millis() - lastStatsTime > 10000) {
        uint32_t polls = usbDeviceProxy.getPollCount();
        Serial4.print("I: USB Device polling rate: ~");
        Serial4.print(polls / 10);  // Rough average per second
        Serial4.println(" Hz");
        lastStatsTime = millis();
    }
}

void updateStatusLED() {
    static unsigned long lastBlink = 0;
    unsigned long now = millis();
    
    // Different blink patterns based on state
    if (!usbMouseHandler.isReady()) {
        // Slow blink - waiting for USB host device (mouse)
        if (now - lastBlink >= 1000) {
            digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
            lastBlink = now;
        }
    } else if (!usbDeviceProxy.isConnected()) {
        // Fast blink - mouse connected but PC not detecting us
        if (now - lastBlink >= 250) {
            digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
            lastBlink = now;
        }
    } else {
        // Solid on - everything connected
        digitalWrite(LED_BUILTIN, HIGH);
    }
}