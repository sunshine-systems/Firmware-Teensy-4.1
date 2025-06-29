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
#include "SunBoxLogger.h"    // Add logger include
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

// =============================================================================
// Function Declarations
// =============================================================================

void updateUSBDeviceStatus();
void updateStatusLED();
void mouseDataCallback(const uint8_t* data, uint32_t length);
void buildEndpointMapping();
void forwardMouseData();

// =============================================================================
// Setup
// =============================================================================

void setup() {
    // Initialize LED
    pinMode(LED_BUILTIN, OUTPUT);
    
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
    logger.startup("Waiting for USB mouse before starting device proxy...");
    
    // Start USB Host
    logger.startup("Starting USB Host...");
    myusb.begin();
    
    // Initialize USB driver
    if (!usbHostDriver.begin()) {
        logger.error("Failed to initialize USB Host Driver!");
        while(1) {
            digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
            delay(100);
        }
    }
    
    systemInitialized = true;
    logger.startup("System initialized - type 'help' for commands");
}

// =============================================================================
// Main Loop
// =============================================================================

void loop() {
    // Process USB Host tasks
    myusb.Task();
    
    // Check if we need to start USB Device Proxy
    if (!usbDeviceProxyStarted && usbMouseHandler.isReady()) {
        logger.startup("Mouse is ready, starting USB Device Proxy now!");
        
        // Get the actual device speed (0=Low, 1=Full, 2=High)
        uint8_t device_speed = usbHostDriver.getDeviceSpeed();
        
        // Determine what speed to configure the proxy for
        bool proxy_high_speed = false;
        
        if (device_speed == 0) {
            // Low Speed device - force Full Speed on proxy side (backwards compatible)
            proxy_high_speed = false;
        } else if (device_speed == 1) {
            // Full Speed device
            proxy_high_speed = false;
        } else {
            // High Speed device
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
    static uint32_t packet_count = 0;  // Move this outside the debug block
    packet_count++;
    
    // Only process if device proxy is configured
    if (!usbDeviceProxyStarted || !usbDeviceProxy.isConfigured()) {
        logger.debug("Mouse data received but proxy not ready");
        return;
    }
    
    // DEBUG: Log when mouse data is received
    // Log every 100th packet
    if ((packet_count % 100) == 1) {
        logger.debugf("Mouse packet #%lu len=%lu data: %02X %02X %02X %02X %02X",
                      packet_count, length,
                      (length > 0) ? data[0] : 0,
                      (length > 1) ? data[1] : 0,
                      (length > 2) ? data[2] : 0,
                      (length > 3) ? data[3] : 0,
                      (length > 4) ? data[4] : 0);
    }
    
    // Forward data immediately if we can
    if (length > 0 && length <= sizeof(mouse_buffer)) {
        // Find the mouse endpoint
        bool found_mouse_ep = false;
        for (int i = 0; i < endpoint_map_count; i++) {
            if (endpoint_map[i].is_mouse) {
                found_mouse_ep = true;
                uint8_t ep_num = endpoint_map[i].ep_num;
                
                if ((packet_count % 100) == 1) {
                    logger.debugf("Found mouse endpoint: EP%d", ep_num);
                }
                
                // Check if endpoint is ready
                if (usbDeviceProxy.isEndpointReady(ep_num)) {
                    // Send immediately
                    usbDeviceProxy.sendDataOnEndpoint(ep_num, data, length);
                    
                    static uint32_t immediate_count = 0;
                    immediate_count++;
                    if ((immediate_count % 100) == 1) {
                        logger.debugf("Forwarded immediately to EP%d", ep_num);
                    }
                } else {
                    // Buffer for later
                    memcpy(mouse_buffer, data, length);
                    mouse_data_len = length;
                    mouse_data_available = true;
                    
                    static uint32_t buffer_count = 0;
                    buffer_count++;
                    if ((buffer_count % 100) == 1) {
                        logger.debugf("EP%d busy, buffered data", ep_num);
                    }
                }
                break;
            }
        }
        
        if (!found_mouse_ep) {
            logger.error("No mouse endpoint in mapping!");
        }
    }
}

// Build endpoint mapping based on parsed configuration
void buildEndpointMapping() {
    endpoint_map_count = 0;
    
    logger.debug("Building endpoint mapping dynamically...");
    
    // Get the endpoint being used by the host driver (includes direction bit)
    uint8_t mouse_endpoint_addr = usbHostDriver.getConfiguredMouseEndpoint();
    uint8_t mouse_ep_num = mouse_endpoint_addr & 0x0F;
    
    logger.debugf("Host driver is using endpoint 0x%02X (EP%d) for mouse data", 
                  mouse_endpoint_addr, mouse_ep_num);
    
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
            logger.debugf("Interface %d (EP%d) identified as mouse - MATCHES HOST DRIVER", 
                         iface_num, ep_num);
        }
        
        // Add to mapping
        endpoint_map[endpoint_map_count].ep_num = ep_num;
        endpoint_map[endpoint_map_count].interface_num = iface_num;
        endpoint_map[endpoint_map_count].is_mouse = is_mouse;
        
        String epInfo = "  EP";
        epInfo += String(ep_num);
        epInfo += " -> Interface ";
        epInfo += String(iface_num);
        epInfo += " (";
        if (is_mouse) {
            epInfo += "Mouse - ACTIVE";
        } else if (iface_class == 0x03) {
            switch (iface_protocol) {
                case 0: epInfo += "HID Other"; break;
                case 1: epInfo += "HID Keyboard"; break;
                case 2: epInfo += "HID Mouse - inactive"; break;
                default: epInfo += "HID Unknown"; break;
            }
        } else {
            epInfo += "Other";
        }
        epInfo += ")";
        logger.debug(epInfo.c_str());
        
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
        logger.error("WARNING - No mouse endpoint identified!");
        // Force mapping for Logitech if detection failed
        if (usbHostDriver.getVendorID() == 0x046D) {
            logger.startup("Forcing EP2 as mouse for Logitech device");
            for (int i = 0; i < endpoint_map_count; i++) {
                if (endpoint_map[i].ep_num == 2) {
                    endpoint_map[i].is_mouse = true;
                    found_mouse = true;
                    break;
                }
            }
        }
    }
    
    logger.debugf("Built endpoint mapping with %d endpoints", endpoint_map_count);
}

// Forward buffered mouse data to the appropriate endpoint
void forwardMouseData() {
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
                
                logger.debugf("Forwarded buffered data to EP%d (endpoint became ready)", ep_num);
                break;
            }
        }
    }
}

// =============================================================================
// Helper Functions
// =============================================================================

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
        logger.debugf("USB Device State: %s", usbDeviceProxy.getStateString());
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
        
        logger.infof("USB Device polling rate: ~%lu Hz", pollRate);
        
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