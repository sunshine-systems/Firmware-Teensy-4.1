// SunshineUSBProxy.ino - Clean Architecture USB Proxy
#include <USBHost_t36.h>
#include "USBHostDriver.h"
#include "HIDMouseDescriptorHandler.h"
#include "SunBoxCommands.h"
#include "SunBoxUSBMouseDataHandler.h"
#include "SunBoxSyntheticHandleOutput.h"
#include "CommandsSunBoxDevtoolsInterface.h"
#include "SunBoxEEPROM.h"
#include "SunBoxStartup.h"  // Add this include
#include "USBDeviceProxy.h"  // NEW: Add our custom USB device stack

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
bool usbDeviceProxyStarted = false;  // Track if device proxy has been started

// Data forwarding buffers
static uint8_t mouse_buffer[64];
static uint32_t mouse_data_len = 0;
static bool mouse_data_available = false;

// Track which endpoints map to which interface
struct EndpointMapping {
    uint8_t ep_num;
    uint8_t interface_num;
    bool is_mouse;  // true for mouse interface, false for keyboard/other
};
static EndpointMapping endpoint_map[8];
static uint8_t endpoint_map_count = 0;

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
    
    // Set up data callback for USB mouse data
    usbHostDriver.setDataCallback(mouseDataCallback);
    
    // NOTE: We do NOT start USBDeviceProxy here anymore!
    Serial4.println("S: Waiting for USB mouse before starting device proxy...");
    
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
    
    // Check if we need to start USB Device Proxy
    if (!usbDeviceProxyStarted && usbMouseHandler.isReady()) {
        Serial4.println("\nS: Mouse is ready, starting USB Device Proxy now!");
        usbDeviceProxy.begin();
        usbDeviceProxyStarted = true;
        
        // Build endpoint mapping based on what we know about the device
        buildEndpointMapping();
        
        // Give it a moment to initialize
        delay(100);
    }
    
    // Only poll USB Device proxy if it's been started
    if (usbDeviceProxyStarted) {
        // Poll USB Device proxy - CRITICAL: Must be called as often as possible!
        usbDeviceProxy.poll();
        
        // Forward mouse data if available and endpoint is ready
        if (mouse_data_available && usbDeviceProxy.isConfigured()) {
            forwardMouseData();
        }
    }
    
    // Check for commands
    sunboxCommands.check();
    
    // Check for USB data
    usbMouseHandler.check();
    
    // Process output if any data is available from serial commands
    if (sunboxCommands.hasData()) {
        syntheticOutput.process();
    }
    
    // Update status based on USB device proxy state
    if (usbDeviceProxyStarted) {
        updateUSBDeviceStatus();
    }
    
    // Update status LED
    updateStatusLED();
}

// =============================================================================
// Data Forwarding
// =============================================================================

// Callback for USB mouse data
void mouseDataCallback(const uint8_t* data, uint32_t length) {
    // Store the data for forwarding
    if (length > 0 && length <= sizeof(mouse_buffer)) {
        memcpy(mouse_buffer, data, length);
        mouse_data_len = length;
        mouse_data_available = true;
    }
}

// Build endpoint mapping based on parsed configuration
void buildEndpointMapping() {
    endpoint_map_count = 0;
    
    // Based on the logs, we know:
    // Interface 0 = HID Mouse (endpoint 0x81, 8 bytes)
    // Interface 1 = HID Keyboard (endpoint 0x82, 64 bytes)  
    // Interface 2 = HID Other (endpoint 0x83, 64 bytes)
    
    // For now, hardcode based on what we see in the logs
    // TODO: Make this dynamic based on interface descriptors
    endpoint_map[0].ep_num = 1;
    endpoint_map[0].interface_num = 0;
    endpoint_map[0].is_mouse = true;
    
    endpoint_map[1].ep_num = 2;
    endpoint_map[1].interface_num = 1;
    endpoint_map[1].is_mouse = false;
    
    endpoint_map[2].ep_num = 3;
    endpoint_map[2].interface_num = 2;
    endpoint_map[2].is_mouse = false;
    
    endpoint_map_count = 3;
    
    Serial4.println("S: Built endpoint mapping:");
    for (int i = 0; i < endpoint_map_count; i++) {
        Serial4.print("S:   EP");
        Serial4.print(endpoint_map[i].ep_num);
        Serial4.print(" -> Interface ");
        Serial4.print(endpoint_map[i].interface_num);
        Serial4.print(" (");
        Serial4.print(endpoint_map[i].is_mouse ? "Mouse" : "Other");
        Serial4.println(")");
    }
}

// Forward mouse data to the appropriate endpoint
void forwardMouseData() {
    if (!mouse_data_available) return;
    
    // Find the mouse endpoint
    for (int i = 0; i < endpoint_map_count; i++) {
        if (endpoint_map[i].is_mouse) {
            uint8_t ep_num = endpoint_map[i].ep_num;
            
            // Check if endpoint is ready
            if (usbDeviceProxy.isEndpointReady(ep_num)) {
                // Send the data
                usbDeviceProxy.sendDataOnEndpoint(ep_num, mouse_buffer, mouse_data_len);
                mouse_data_available = false;
                
                // Debug output
                if (SunBoxStartup::isDebugEnabled()) {
                    Serial4.print("I: Forwarded ");
                    Serial4.print(mouse_data_len);
                    Serial4.print(" bytes to EP");
                    Serial4.println(ep_num);
                }
                break;
            }
        }
    }
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
    Serial4.println("I:   - Custom USB Device Stack (Polling)");
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
    } else if (!usbDeviceProxyStarted || !usbDeviceProxy.isConnected()) {
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