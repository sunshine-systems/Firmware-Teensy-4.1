// SunshineUSBProxy.ino - Clean Architecture USB Proxy
#include <USBHost_t36.h>
#include "USBHostDriver.h"
#include "HIDMouseDescriptorHandler.h"
#include "SunBoxCommands.h"
#include "SunBoxUSBMouseDataHandler.h"
#include "SunBoxSyntheticHandleOutput.h"
#include "CommandsSunBoxDevtoolsInterface.h"
#include "SunBoxEEPROM.h"

// USB device functions
extern "C" {
    #include "usb_dev.h"
    void usb_init(void);
}

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
    
    // Check for commands
    sunboxCommands.check();
    
    // Check for USB data
    usbMouseHandler.check();
    
    // Process output if any data is available
    if (sunboxCommands.hasData() || usbMouseHandler.hasData()) {
        syntheticOutput.process();
    }
    
    // Initialize USB device when ready
    initializeUSBDevice();
    
    // Update status LED
    updateStatusLED();
}

// =============================================================================
// Helper Functions
// =============================================================================

void printBanner() {
    Serial4.println("\n\n");
    Serial4.println("=======================================");
    Serial4.println("       SunBox USB Proxy v3.0");
    Serial4.println("=======================================");
    Serial4.println("Clean Architecture Implementation");
    Serial4.println("Features:");
    Serial4.println("  - USB HID Mouse Proxy");
    Serial4.println("  - Sunshine Legacy Protocol");
    Serial4.println("  - KMBox B+ Protocol");
    Serial4.println("  - Intelligent Command Routing");
    Serial4.println("=======================================\n");
}

void initializeUSBDevice() {
    // Only initialize USB device after we know what we're proxying
    static bool usbDeviceInitialized = false;
    static unsigned long lastCheckTime = 0;
    
    if (!usbDeviceInitialized && (millis() - lastCheckTime > 1000)) {
        lastCheckTime = millis();
        
        if (usbMouseHandler.isReady()) {
            Serial4.println("S: USB device detected and ready");
            Serial4.println("S: Initializing USB device stack...");
            
            // Initialize USB device stack
            usb_init();
            
            usbDeviceInitialized = true;
            Serial4.println("S: USB device stack initialized");
            
            // Flash LED to indicate ready
            for (int i = 0; i < 5; i++) {
                digitalWrite(LED_BUILTIN, HIGH);
                delay(100);
                digitalWrite(LED_BUILTIN, LOW);
                delay(100);
            }
        }
    }
}

void updateStatusLED() {
    static unsigned long lastBlink = 0;
    unsigned long now = millis();
    
    if (!usbMouseHandler.isReady()) {
        // Slow blink - waiting for device
        if (now - lastBlink >= 1000) {
            digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
            lastBlink = now;
        }
    } else {
        // Solid on - device connected
        digitalWrite(LED_BUILTIN, HIGH);
    }
}