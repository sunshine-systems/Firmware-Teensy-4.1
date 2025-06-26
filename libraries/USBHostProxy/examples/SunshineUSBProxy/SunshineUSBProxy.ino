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

// Vendor command simulation
struct VendorCommand {
    uint32_t nextTime;
    uint8_t sequence;
    bool active;
};
static VendorCommand vendorSim = {0, 0, false};

// =============================================================================
// Function Declarations
// =============================================================================

void printBanner();
void updateUSBDeviceStatus();
void updateStatusLED();
void mouseDataCallback(const uint8_t* data, uint32_t length);
void buildEndpointMapping();
void forwardMouseData();
void simulateVendorCommands();

// =============================================================================
// Setup
// =============================================================================

void setup() {
    // Initialize LED
    pinMode(LED_BUILTIN, OUTPUT);
    
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
        
        // Get the actual device speed (0=Low, 1=Full, 2=High)
        uint8_t device_speed = usbHostDriver.getDeviceSpeed();
        
        // Determine what speed to configure the proxy for
        bool proxy_high_speed = false;
        
        if (device_speed == 0) {
            // Low Speed device - force Full Speed on proxy side (backwards compatible)
            Serial4.println("S: Low Speed device detected - forcing Full Speed proxy");
            proxy_high_speed = false;
        } else if (device_speed == 1) {
            // Full Speed device
            Serial4.println("S: Full Speed device detected");
            proxy_high_speed = false;
        } else {
            // High Speed device
            Serial4.println("S: High Speed device detected");
            proxy_high_speed = true;
        }
        
        // Configure the proxy to match (Low Speed devices run at Full Speed)
        usbDeviceProxy.setDeviceSpeed(proxy_high_speed);
        
        // Set the USB Host Driver reference
        usbDeviceProxy.setUSBHostDriver(&usbHostDriver);
        
        // Start the device proxy
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
        
        // Process any buffered mouse data
        if (mouse_data_available) {
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
    
    // Simulate vendor commands if enabled
    simulateVendorCommands();
    
    // Update status based on USB device proxy state
    if (usbDeviceProxyStarted) {
        updateUSBDeviceStatus();
    }
    
    // Update status LED
    updateStatusLED();
}

// =============================================================================
// Vendor Command Simulation
// =============================================================================

void simulateVendorCommands() {
    if (!vendorSim.active) return;
    if (!usbHostDriver.isReady()) return;
    if (millis() < vendorSim.nextTime) return;
    
    // Simulate the vendor command sequence from the logs
    static const uint8_t commands[][6] = {
        {0x00, 0x00, 0x00, 0x06, 0x00, 0x81},  // First command
        {0x00, 0x00, 0x02, 0x01, 0x00, 0x85},  // Second command
        {0x00, 0x00, 0x02, 0x02, 0x01, 0x80},  // Third command
        {0x00, 0x00, 0x02, 0x1A, 0x01, 0x81},  // Fourth command
        {0x00, 0x00, 0x02, 0x02, 0x00, 0x83},  // Fifth command
        {0x00, 0x00, 0x02, 0x03, 0x00, 0x87},  // Sixth command
        {0x00, 0x00, 0x02, 0x06, 0x00, 0x81},  // Seventh command
        {0x00, 0x00, 0x02, 0x13, 0x02, 0x81},  // Eighth command
    };
    
    if (vendorSim.sequence >= sizeof(commands)/sizeof(commands[0])) {
        vendorSim.sequence = 0;
    }
    
    // Build the 64-byte command
    uint8_t cmdData[64] = {0};
    memcpy(cmdData, commands[vendorSim.sequence], 6);
    
    Serial4.print("\nS: Vendor Sim: Sending command ");
    Serial4.print(vendorSim.sequence + 1);
    Serial4.print("/");
    Serial4.print(sizeof(commands)/sizeof(commands[0]));
    Serial4.print(": ");
    for (int i = 0; i < 6; i++) {
        if (cmdData[i] < 0x10) Serial4.print("0");
        Serial4.print(cmdData[i], HEX);
        Serial4.print(" ");
    }
    Serial4.println();
    
    // Send SET_REPORT
    usbHostDriver.pauseDataTransfers();
    
    uint16_t actualLen = 0;
    bool success = usbHostDriver.controlTransfer(
        0x21, 0x09, 0x0300, 0x0002, 64,
        cmdData, &actualLen, 100
    );
    
    if (success) {
        Serial4.println("S: SET_REPORT OK");
        
        // Get response
        uint8_t response[64];
        success = usbHostDriver.controlTransfer(
            0xA1, 0x01, 0x0300, 0x0002, 64,
            response, &actualLen, 100
        );
        
        if (success && actualLen > 0) {
            Serial4.print("S: GET_REPORT response (");
            Serial4.print(actualLen);
            Serial4.print(" bytes): ");
            for (int i = 0; i < 16 && i < actualLen; i++) {
                if (response[i] < 0x10) Serial4.print("0");
                Serial4.print(response[i], HEX);
                Serial4.print(" ");
            }
            Serial4.println();
        }
    } else {
        Serial4.println("E: SET_REPORT failed");
    }
    
    usbHostDriver.resumeDataTransfers();
    
    // Next command in 50ms
    vendorSim.sequence++;
    vendorSim.nextTime = millis() + 50;
}

// =============================================================================
// Data Forwarding
// =============================================================================

// Callback for USB mouse data
void mouseDataCallback(const uint8_t* data, uint32_t length) {
    bool debug_enabled = SunBoxStartup::isDebugEnabled();
    static uint32_t packet_count = 0;  // Move this outside the debug block
    packet_count++;
    
    // Only process if device proxy is configured
    if (!usbDeviceProxyStarted || !usbDeviceProxy.isConfigured()) {
        if (debug_enabled) {
            Serial4.println("D: Mouse data received but proxy not ready");
        }
        return;
    }
    
    // DEBUG: Log when mouse data is received
    if (debug_enabled) {
        // Log every 100th packet
        if ((packet_count % 100) == 1) {
            Serial4.print("D: Mouse packet #");
            Serial4.print(packet_count);
            Serial4.print(" len=");
            Serial4.print(length);
            Serial4.print(" data: ");
            for (uint32_t i = 0; i < 5 && i < length; i++) {
                if (data[i] < 0x10) Serial4.print("0");
                Serial4.print(data[i], HEX);
                Serial4.print(" ");
            }
            Serial4.println();
        }
    }
    
    // Forward data immediately if we can
    if (length > 0 && length <= sizeof(mouse_buffer)) {
        // Find the mouse endpoint
        bool found_mouse_ep = false;
        for (int i = 0; i < endpoint_map_count; i++) {
            if (endpoint_map[i].is_mouse) {
                found_mouse_ep = true;
                uint8_t ep_num = endpoint_map[i].ep_num;
                
                if (debug_enabled && (packet_count % 100) == 1) {
                    Serial4.print("D: Found mouse endpoint: EP");
                    Serial4.println(ep_num);
                }
                
                // Check if endpoint is ready
                if (usbDeviceProxy.isEndpointReady(ep_num)) {
                    // Send immediately
                    usbDeviceProxy.sendDataOnEndpoint(ep_num, data, length);
                    
                    if (debug_enabled) {
                        static uint32_t immediate_count = 0;
                        immediate_count++;
                        if ((immediate_count % 100) == 1) {
                            Serial4.print("D: Forwarded immediately to EP");
                            Serial4.println(ep_num);
                        }
                    }
                } else {
                    // Buffer for later
                    memcpy(mouse_buffer, data, length);
                    mouse_data_len = length;
                    mouse_data_available = true;
                    
                    if (debug_enabled) {
                        static uint32_t buffer_count = 0;
                        buffer_count++;
                        if ((buffer_count % 100) == 1) {
                            Serial4.print("D: EP");
                            Serial4.print(ep_num);
                            Serial4.println(" busy, buffered data");
                        }
                    }
                }
                break;
            }
        }
        
        if (!found_mouse_ep && debug_enabled) {
            Serial4.println("E: No mouse endpoint in mapping!");
        }
    }
}

// Build endpoint mapping based on parsed configuration
void buildEndpointMapping() {
    endpoint_map_count = 0;
    
    Serial4.println("S: Building endpoint mapping dynamically...");
    
    // Get the endpoint being used by the host driver (includes direction bit)
    uint8_t mouse_endpoint_addr = usbHostDriver.getConfiguredMouseEndpoint();
    uint8_t mouse_ep_num = mouse_endpoint_addr & 0x0F;
    
    Serial4.print("S: Host driver is using endpoint 0x");
    Serial4.print(mouse_endpoint_addr, HEX);
    Serial4.print(" (EP");
    Serial4.print(mouse_ep_num);
    Serial4.println(") for mouse data");
    
    // Get interface information from the host driver
    uint8_t num_interfaces = usbHostDriver.getInterfaceCount();
    
    for (uint8_t i = 0; i < num_interfaces && endpoint_map_count < 8; i++) {
        uint8_t iface_num = usbHostDriver.getInterfaceNumber(i);
        uint8_t iface_class = usbHostDriver.getInterfaceClass(i);
        uint8_t iface_protocol = usbHostDriver.getInterfaceProtocol(i);
        uint8_t ep_addr = usbHostDriver.getEndpointAddress(i);
        
        if (ep_addr == 0) continue; // Skip interfaces without endpoints
        
        // ep_addr from getEndpointAddress() is just the number (no direction bit)
        uint8_t ep_num = ep_addr;
        
        // Determine if this is the mouse interface
        bool is_mouse = false;
        
        // Check if this endpoint number matches what the host driver is using
        if (ep_num == mouse_ep_num) {
            is_mouse = true;
            Serial4.print("S: Interface ");
            Serial4.print(iface_num);
            Serial4.print(" (EP");
            Serial4.print(ep_num);
            Serial4.println(") identified as mouse - MATCHES HOST DRIVER");
        }
        
        // Add to mapping
        endpoint_map[endpoint_map_count].ep_num = ep_num;
        endpoint_map[endpoint_map_count].interface_num = iface_num;
        endpoint_map[endpoint_map_count].is_mouse = is_mouse;
        
        Serial4.print("S:   EP");
        Serial4.print(ep_num);
        Serial4.print(" -> Interface ");
        Serial4.print(iface_num);
        Serial4.print(" (");
        if (is_mouse) {
            Serial4.print("Mouse - ACTIVE");
        } else if (iface_class == 0x03) {
            switch (iface_protocol) {
                case 0: Serial4.print("HID Other"); break;
                case 1: Serial4.print("HID Keyboard"); break;
                case 2: Serial4.print("HID Mouse - inactive"); break;
                default: Serial4.print("HID Unknown"); break;
            }
        } else {
            Serial4.print("Other");
        }
        Serial4.println(")");
        
        endpoint_map_count++;
    }
    
    // Verify we found the mouse endpoint
    bool found_mouse = false;
    for (int i = 0; i < endpoint_map_count; i++) {
        if (endpoint_map[i].is_mouse) {
            found_mouse = true;
            break;
        }
    }
    
    if (!found_mouse) {
        Serial4.println("E: WARNING - No mouse endpoint identified!");
        // Force mapping for Logitech if detection failed
        if (usbHostDriver.getVendorID() == 0x046D) {
            Serial4.println("E: Forcing EP2 as mouse for Logitech device");
            for (int i = 0; i < endpoint_map_count; i++) {
                if (endpoint_map[i].ep_num == 2) {
                    endpoint_map[i].is_mouse = true;
                    found_mouse = true;
                    break;
                }
            }
        }
    }
    
    Serial4.print("S: Built endpoint mapping with ");
    Serial4.print(endpoint_map_count);
    Serial4.println(" endpoints");
}

// Forward buffered mouse data to the appropriate endpoint
void forwardMouseData() {
    bool debug_enabled = SunBoxStartup::isDebugEnabled();
    
    if (!mouse_data_available) return;
    
    // Find the mouse endpoint
    for (int i = 0; i < endpoint_map_count; i++) {
        if (endpoint_map[i].is_mouse) {
            uint8_t ep_num = endpoint_map[i].ep_num;
            
            // Check if endpoint is ready
            if (usbDeviceProxy.isEndpointReady(ep_num)) {
                // Send the buffered data
                usbDeviceProxy.sendDataOnEndpoint(ep_num, mouse_buffer, mouse_data_len);
                mouse_data_available = false;
                
                if (debug_enabled) {
                    Serial4.print("D: Forwarded buffered data to EP");
                    Serial4.print(ep_num);
                    Serial4.println(" (endpoint became ready)");
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
    static uint32_t lastPollCount = 0;
    
    if (millis() - lastStatsTime > 10000) {
        uint32_t currentPollCount = usbDeviceProxy.getPollCount();
        uint32_t pollsDelta = currentPollCount - lastPollCount;
        uint32_t timeDelta = millis() - lastStatsTime;
        
        // Calculate actual rate (polls per second)
        uint32_t pollRate = (pollsDelta * 1000) / timeDelta;
        
        Serial4.print("I: USB Device polling rate: ~");
        Serial4.print(pollRate);
        Serial4.println(" Hz");
        
        lastPollCount = currentPollCount;
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