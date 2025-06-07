// USBDeviceProxy.cpp - USB Device Stack for Teensy 4.1 (Polling-based)
#include "USBDeviceProxy.h"
#include "usb_host_wrapper.h"
#include "imxrt.h"     // For i.MX RT1062 definitions
#include "usb_dev.h"   // For USB register definitions
#include "core_pins.h" // For CCM and other peripheral definitions
#include <Arduino.h>

// USB memory buffers (must be aligned)
// These will be used in Phase 3 for control transfers
static uint8_t endpoint0_buffer[64] __attribute__((aligned(32)));
static uint8_t descriptor_buffer[512] __attribute__((aligned(32)));

// Queue heads for all endpoints (must be 4K aligned)
__attribute__((aligned(4096)))
static proxy_endpoint_queue_head_t proxy_endpoint_queue_head[16];  // 8 endpoints * 2 directions

// Constructor
USBDeviceProxy::USBDeviceProxy() : 
    device_state(STATE_DETACHED),
    device_address(0),
    configuration_value(0),
    phy_initialized(false),
    controller_started(false),
    setup_received(false),
    last_poll_time(0),
    poll_count(0) {
}

// Initialize USB hardware
void USBDeviceProxy::begin() {
    Serial4.println("S: USBDeviceProxy::begin() - Initializing USB device hardware");
    
    // Initialize PHY
    if (!initializePHY()) {
        Serial4.println("E: Failed to initialize USB PHY!");
        return;
    }
    
    // Initialize controller
    if (!initializeController()) {
        Serial4.println("E: Failed to initialize USB controller!");
        return;
    }
    
    // Initialize endpoint structures
    initializeEndpoints();
    
    // Start the controller
    startController();
    
    Serial4.println("S: USB Device hardware initialized (polling mode)");
}

// Main polling function - MUST be called frequently from loop()
void USBDeviceProxy::poll() {
    // Track polling rate for debugging
    uint32_t now = micros();
    if (last_poll_time != 0) {
        uint32_t delta = now - last_poll_time;
        if (delta > 100) {  // More than 100us since last poll
            // We're polling too slowly!
            if ((poll_count % 10000) == 0) {
                Serial4.print("W: Slow polling detected: ");
                Serial4.print(delta);
                Serial4.println("us");
            }
        }
    }
    last_poll_time = now;
    poll_count++;
    
    // Check if controller is ready
    if (!controller_started) {
        return;
    }
    
    // Read and clear status register
    uint32_t status = USB1_USBSTS;
    if (status) {
        USB1_USBSTS = status;  // Clear by writing 1s
        
        // Handle status bits
        if (status & USB_USBSTS_UI) {
            handleUSBInterrupt();
        }
        
        if (status & USB_USBSTS_URI) {
            handleUSBReset();
        }
        
        if (status & USB_USBSTS_PCI) {
            handlePortChange();
        }
        
        if (status & USB_USBSTS_UEI) {
            Serial4.println("E: USB Error detected!");
        }
    }
    
    // Always check for setup packets (high priority)
    pollControlEndpoint();
}

// Initialize USB PHY
bool USBDeviceProxy::initializePHY() {
    Serial4.println("I: Initializing USB PHY...");
    
    // 1. Power configuration
    PMU_REG_3P0 = PMU_REG_3P0_OUTPUT_TRG(0x0F) | PMU_REG_3P0_BO_OFFSET(6)
        | PMU_REG_3P0_ENABLE_LINREG;
    
    // 2. Enable clocks
    CCM_CCGR6 |= CCM_CCGR6_USBOH3(CCM_CCGR_ON);
    
    // 3. Check if we need to reset
    if ((USBPHY1_PWD & (USBPHY_PWD_RXPWDRX | USBPHY_PWD_RXPWDDIFF | USBPHY_PWD_RXPWD1PT1
      | USBPHY_PWD_RXPWDENV | USBPHY_PWD_TXPWDV2I | USBPHY_PWD_TXPWDIBIAS
      | USBPHY_PWD_TXPWDFS)) || (USB1_USBMODE & USB_USBMODE_CM_MASK)) {
        
        Serial4.println("I: PHY needs reset");
        
        // Reset PHY
        USBPHY1_CTRL_SET = USBPHY_CTRL_SFTRST;
        
        // Reset controller
        USB1_USBCMD |= USB_USBCMD_RST;
        int count = 0;
        while (USB1_USBCMD & USB_USBCMD_RST) {
            count++;
            if (count > 100000) {
                Serial4.println("E: USB reset timeout!");
                return false;
            }
        }
        
        Serial4.print("I: USB reset took ");
        Serial4.print(count);
        Serial4.println(" loops");
        
        // Clear reset
        USBPHY1_CTRL_CLR = USBPHY_CTRL_SFTRST;
        
        // Wait for PHY to stabilize
        delay(25);
    }
    
    // 4. Power up PHY
    USBPHY1_CTRL_CLR = USBPHY_CTRL_CLKGATE;
    USBPHY1_PWD = 0;  // Power up all blocks
    
    Serial4.println("I: USB PHY initialized");
    phy_initialized = true;
    return true;
}

// Initialize USB controller
bool USBDeviceProxy::initializeController() {
    Serial4.println("I: Initializing USB controller...");
    
    // 1. Set device mode
    USB1_USBMODE = USB_USBMODE_CM(2) | USB_USBMODE_SLOM;
    
    // 2. Set up burst size
    USB1_BURSTSIZE = 0x0404;
    
    // 3. Clear all pending interrupts (we won't enable them)
    USB1_USBSTS = USB1_USBSTS;
    
    // 4. DON'T enable interrupts - this is key!
    // Traditional stack would do: USB1_USBINTR = ...
    // We explicitly leave interrupts disabled
    USB1_USBINTR = 0;
    
    Serial4.println("I: USB controller initialized (interrupts disabled)");
    controller_started = true;
    return true;
}

// Initialize endpoint structures
void USBDeviceProxy::initializeEndpoints() {
    Serial4.println("I: Initializing endpoints...");
    
    // Clear all queue heads
    memset(proxy_endpoint_queue_head, 0, sizeof(proxy_endpoint_queue_head));
    
    // Clear buffers too (to avoid unused warnings)
    memset(endpoint0_buffer, 0, sizeof(endpoint0_buffer));
    memset(descriptor_buffer, 0, sizeof(descriptor_buffer));
    
    // Configure endpoint 0 (control)
    proxy_endpoint_queue_head[0].config = (64 << 16) | (1 << 15);  // RX: 64 bytes, control endpoint
    proxy_endpoint_queue_head[1].config = (64 << 16) | (1 << 15);  // TX: 64 bytes, control endpoint
    
    // Set endpoint list address
    USB1_ENDPOINTLISTADDR = (uint32_t)proxy_endpoint_queue_head;
    
    Serial4.println("I: Endpoints initialized");
}

// Start USB controller
void USBDeviceProxy::startController() {
    Serial4.println("I: Starting USB controller...");
    
    // Enable pull-up resistor and run
    USB1_USBCMD = USB_USBCMD_RS;
    
    // Update state
    device_state = STATE_ATTACHED;
    
    Serial4.println("I: USB controller started - device should enumerate");
}

// Poll control endpoint for setup packets
void USBDeviceProxy::pollControlEndpoint() {
    // Check for setup packet
    uint32_t setup_status = USB1_ENDPTSETUPSTAT;
    
    if (setup_status & 1) {  // Setup packet on endpoint 0
        // Read setup packet
        uint32_t setup0, setup1;
        
        // Must use this sequence to read setup packet atomically
        do {
            USB1_USBCMD |= USB_USBCMD_SUTW;
            setup0 = proxy_endpoint_queue_head[0].setup0;
            setup1 = proxy_endpoint_queue_head[0].setup1;
        } while (!(USB1_USBCMD & USB_USBCMD_SUTW));
        
        USB1_USBCMD &= ~USB_USBCMD_SUTW;
        
        // Clear the setup status
        USB1_ENDPTSETUPSTAT = setup_status;
        
        // Flush any pending transfers
        USB1_ENDPTFLUSH = (1 << 16) | (1 << 0);  // Flush EP0 IN and OUT
        while (USB1_ENDPTFLUSH & ((1 << 16) | (1 << 0)));
        
        // Handle the setup packet
        handleSetupPacket(setup0, setup1);
    }
}

// Handle USB interrupt status (called from poll)
void USBDeviceProxy::handleUSBInterrupt() {
    // Check endpoint complete status
    uint32_t complete = USB1_ENDPTCOMPLETE;
    if (complete) {
        USB1_ENDPTCOMPLETE = complete;
        
        // For now, just track that transfers completed
        if (complete & 0x10000) {  // EP0 TX complete
            // Control IN transfer completed
        }
        if (complete & 0x00001) {  // EP0 RX complete  
            // Control OUT transfer completed
        }
    }
}

// Handle USB reset
void USBDeviceProxy::handleUSBReset() {
    Serial4.println("S: USB Reset detected");
    
    // Clear all status
    USB1_ENDPTSETUPSTAT = USB1_ENDPTSETUPSTAT;
    USB1_ENDPTCOMPLETE = USB1_ENDPTCOMPLETE;
    
    // Wait for prime bits to clear
    while (USB1_ENDPTPRIME != 0);
    
    // Flush all endpoints
    USB1_ENDPTFLUSH = 0xFFFFFFFF;
    
    // Reset device address
    USB1_DEVICEADDR = 0;
    device_address = 0;
    
    // Reset state
    device_state = STATE_DEFAULT;
    configuration_value = 0;
}

// Handle port change
void USBDeviceProxy::handlePortChange() {
    // Check speed
    if (USB1_PORTSC1 & USB_PORTSC1_HSP) {
        Serial4.println("I: High-speed device operation");
    } else {
        Serial4.println("I: Full-speed device operation");
    }
}

// Handle setup packet
void USBDeviceProxy::handleSetupPacket(uint32_t setup0, uint32_t setup1) {
    // Extract setup packet fields
    uint8_t bmRequestType = setup0 & 0xFF;
    uint8_t bRequest = (setup0 >> 8) & 0xFF;
    uint16_t wValue = (setup0 >> 16) & 0xFFFF;
    uint16_t wIndex = setup1 & 0xFFFF;
    uint16_t wLength = (setup1 >> 16) & 0xFFFF;
    
    Serial4.print("S: SETUP: bmRequestType=0x");
    Serial4.print(bmRequestType, HEX);
    Serial4.print(" bRequest=0x");
    Serial4.print(bRequest, HEX);
    Serial4.print(" wValue=0x");
    Serial4.print(wValue, HEX);
    Serial4.print(" wIndex=0x");
    Serial4.print(wIndex, HEX);
    Serial4.print(" wLength=");
    Serial4.println(wLength);
    
    // For now, just STALL everything
    // Phase 3 will implement proper forwarding using endpoint0_buffer and descriptor_buffer
    Serial4.println("I: Phase 2 - STALLing all requests (normal for this phase)");
    
    // Stall both IN and OUT
    USB1_ENDPTCTRL0 = 0x00010001;
}