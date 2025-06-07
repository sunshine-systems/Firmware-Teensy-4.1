// USBDeviceProxy.cpp - USB Device Stack for Teensy 4.1 (Polling-based)
#include "USBDeviceProxy.h"
#include "usb_host_wrapper.h"
#include "imxrt.h"     // For i.MX RT1062 definitions
#include "core_pins.h" // For CCM and other peripheral definitions
#include <Arduino.h>

// =============================================================================
// CRITICAL: Use UNIQUE symbol names to avoid conflict with usb.c!
// =============================================================================

// Main endpoint queue head array - UNIQUE NAME to avoid conflict!
endpoint_t proxy_endpoint_queue_head[(NUM_ENDPOINTS+1)*2] __attribute__ ((used, aligned(4096), section(".endpoint_queue")));

// Transfer structures for endpoint 0 - UNIQUE NAMES
transfer_t proxy_endpoint0_transfer_data __attribute__ ((used, aligned(32)));
transfer_t proxy_endpoint0_transfer_ack  __attribute__ ((used, aligned(32)));

// State variables (our own copies)
static uint32_t proxy_endpoint0_notify_mask=0;
static uint32_t proxy_endpointN_notify_mask=0;
volatile uint8_t proxy_usb_configuration = 0;
volatile uint8_t proxy_usb_high_speed = 0;

// Buffers
static uint8_t proxy_endpoint0_buffer[8];

// Our additional buffers for larger transfers
static uint8_t proxy_descriptor_buffer[512] __attribute__((aligned(32)));

// =============================================================================
// USBDeviceProxy Implementation
// =============================================================================

// Constructor - initialize in declaration order to avoid warnings
USBDeviceProxy::USBDeviceProxy() : 
    device_state(STATE_DETACHED),
    device_address(0),
    configuration_value(0),
    phy_initialized(false),
    controller_started(false),
    setup_received(false),
    control_stage(CONTROL_IDLE),
    setup_data_len(0),
    last_poll_time(0),
    poll_count(0),
    endpoint0_notify_mask(0),
    endpointN_notify_mask(0) {
    
    memset(&pending_setup, 0, sizeof(pending_setup));
}

// Initialize USB hardware - following usb_init() exactly
void USBDeviceProxy::begin() {
    Serial4.println("S: USBDeviceProxy::begin() - Initializing USB device hardware");
    
    // 1. Power configuration (from usb.c line 575-577)
    PMU_REG_3P0 = PMU_REG_3P0_OUTPUT_TRG(0x0F) | PMU_REG_3P0_BO_OFFSET(6)
        | PMU_REG_3P0_ENABLE_LINREG;
    
    // 2. Initialize serial number (we'll skip this as we're proxying)
    // usb_init_serialnumber();
    
    // 3. Enable clocks (from usb.c line 579)
    CCM_CCGR6 |= CCM_CCGR6_USBOH3(CCM_CCGR_ON);
    
    // 4. Set burst size (from usb.c line 583)
    USB1_BURSTSIZE = 0x0404;
    
    // 5. PHY reset if needed (from usb.c line 586-602)
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
        }
        Serial4.print("I: USB reset took ");
        Serial4.print(count);
        Serial4.println(" loops");
        
        // Clear pending interrupt
        NVIC_CLEAR_PENDING(IRQ_USB1);
        
        // Clear reset
        USBPHY1_CTRL_CLR = USBPHY_CTRL_SFTRST;
        
        // Wait for PHY to stabilize
        delay(25);
    }
    
    // 6. Power up PHY (from usb.c line 606-607)
    USBPHY1_CTRL_CLR = USBPHY_CTRL_CLKGATE;
    USBPHY1_PWD = 0;
    
    // 7. Set device mode (from usb.c line 613)
    USB1_USBMODE = USB_USBMODE_CM(2) | USB_USBMODE_SLOM;
    
    // 8. Initialize endpoint queue heads (from usb.c line 615-620)
    memset(proxy_endpoint_queue_head, 0, sizeof(proxy_endpoint_queue_head));
    proxy_endpoint_queue_head[0].config = (64 << 16) | (1 << 15);  // EP0 OUT
    proxy_endpoint_queue_head[1].config = (64 << 16);              // EP0 IN (no control bit!)
    
    // 9. Set endpoint list address - CRITICAL: Use OUR queue heads!
    USB1_ENDPOINTLISTADDR = (uint32_t)&proxy_endpoint_queue_head;
    
    // DEBUG: Verify the address
    Serial4.print("I: proxy_endpoint_queue_head address: 0x");
    Serial4.println((uint32_t)proxy_endpoint_queue_head, HEX);
    Serial4.print("I: USB1_ENDPOINTLISTADDR set to: 0x");
    Serial4.println(USB1_ENDPOINTLISTADDR, HEX);
    
    // 10. Initialize all other endpoints (from usb.c line 627-637)
    for (int i=2; i < (NUM_ENDPOINTS+1)*2; i++) {
        proxy_endpoint_queue_head[i].config = (1<<30) | (64 << 16);
    }
    
    // 11. Clear all pending interrupts
    USB1_USBSTS = USB1_USBSTS;
    
    // 12. DON'T enable interrupts - this is our key difference!
    USB1_USBINTR = 0;
    
    // 13. Start controller (from usb.c line 644)
    USB1_USBCMD = USB_USBCMD_RS;
    
    // Update state
    device_state = STATE_ATTACHED;
    controller_started = true;
    phy_initialized = true;
    
    Serial4.println("S: USB Device hardware initialized (polling mode)");
}

// Main polling function - MUST be called frequently from loop()
void USBDeviceProxy::poll() {
    // Track polling rate for debugging
    uint32_t now = micros();
    if (last_poll_time != 0) {
        uint32_t delta = now - last_poll_time;
        if (delta > 100) {  // More than 100us since last poll
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
    
    // Process any pending control transfers
    processControlTransfer();
}

// Poll control endpoint for setup packets
void USBDeviceProxy::pollControlEndpoint() {
    // Check for setup packet
    uint32_t setupstatus = USB1_ENDPTSETUPSTAT;
    
    while (setupstatus) {
        USB1_ENDPTSETUPSTAT = setupstatus;
        setup_packet_t s;
        
        // Read setup packet atomically (from usb.c)
        do {
            USB1_USBCMD |= USB_USBCMD_SUTW;
            uint32_t setup0 = proxy_endpoint_queue_head[0].setup0;
            uint32_t setup1 = proxy_endpoint_queue_head[0].setup1;
            
            s.bmRequestType = setup0 & 0xFF;
            s.bRequest = (setup0 >> 8) & 0xFF;
            s.wValue = (setup0 >> 16) & 0xFFFF;
            s.wIndex = setup1 & 0xFFFF;
            s.wLength = (setup1 >> 16) & 0xFFFF;
        } while (!(USB1_USBCMD & USB_USBCMD_SUTW));
        
        USB1_USBCMD &= ~USB_USBCMD_SUTW;
        
        // Flush any pending transfers
        USB1_ENDPTFLUSH = (1<<16) | (1<<0);
        while (USB1_ENDPTFLUSH & ((1<<16) | (1<<0)));
        
        // Clear notify mask
        proxy_endpoint0_notify_mask = 0;
        endpoint0_notify_mask = proxy_endpoint0_notify_mask;
        
        // Store and handle the setup packet
        pending_setup = s;
        handleSetupPacket(s.bmRequestType | (s.bRequest << 8), s.wValue | (s.wIndex << 16));
        
        setupstatus = USB1_ENDPTSETUPSTAT;
    }
}

// Handle USB interrupt status (called from poll)
void USBDeviceProxy::handleUSBInterrupt() {
    // Check endpoint complete status
    uint32_t completestatus = USB1_ENDPTCOMPLETE;
    if (completestatus) {
        USB1_ENDPTCOMPLETE = completestatus;
        
        // Debug output
        if (completestatus != 0) {
            Serial4.print("I: ENDPTCOMPLETE = 0x");
            Serial4.println(completestatus, HEX);
        }
        
        // Handle endpoint 0 completions
        if (completestatus & proxy_endpoint0_notify_mask) {
            proxy_endpoint0_notify_mask = 0;
            endpoint0_notify_mask = 0;
            // Handle completion based on control stage
            if (control_stage == CONTROL_DATA_IN) {
                Serial4.println("I: EP0 TX complete");
                control_stage = CONTROL_STATUS_OUT;
                // Prepare to receive status
                receiveData(proxy_endpoint0_buffer, 0);
            }
            else if (control_stage == CONTROL_STATUS_OUT) {
                Serial4.println("I: Control transfer complete!");
                control_stage = CONTROL_IDLE;
                usb_host_resume_transfers();
            }
        }
        
        // Handle other endpoint completions if needed
        completestatus &= proxy_endpointN_notify_mask;
        if (completestatus) {
            // Process other endpoints here if needed
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
    
    // Reset masks
    proxy_endpoint0_notify_mask = 0;
    proxy_endpointN_notify_mask = 0;
    endpoint0_notify_mask = 0;
    endpointN_notify_mask = 0;
    
    // Reset device address
    USB1_DEVICEADDR = 0;
    device_address = 0;
    
    // Reset state
    device_state = STATE_DEFAULT;
    configuration_value = 0;
    proxy_usb_configuration = 0;
    
    // If we were in the middle of a control transfer, resume host transfers
    if (control_stage != CONTROL_IDLE) {
        control_stage = CONTROL_IDLE;
        usb_host_resume_transfers();
    }
}

// Handle port change
void USBDeviceProxy::handlePortChange() {
    // Check speed
    if (USB1_PORTSC1 & USB_PORTSC1_HSP) {
        Serial4.println("I: High-speed device operation");
        proxy_usb_high_speed = 1;
    } else {
        Serial4.println("I: Full-speed device operation");
        proxy_usb_high_speed = 0;
    }
}

// Handle setup packet
void USBDeviceProxy::handleSetupPacket(uint32_t setup0, uint32_t setup1) {
    Serial4.print("S: SETUP: bmRequestType=0x");
    Serial4.print(pending_setup.bmRequestType, HEX);
    Serial4.print(" bRequest=0x");
    Serial4.print(pending_setup.bRequest, HEX);
    Serial4.print(" wValue=0x");
    Serial4.print(pending_setup.wValue, HEX);
    Serial4.print(" wIndex=0x");
    Serial4.print(pending_setup.wIndex, HEX);
    Serial4.print(" wLength=");
    Serial4.println(pending_setup.wLength);
    
    // Check if we have a connected device to proxy
    if (!usb_host_is_ready()) {
        Serial4.println("E: No USB device connected to proxy!");
        USB1_ENDPTCTRL0 = 0x00010001;  // STALL both directions
        return;
    }
    
    // Pause USB host data transfers during control transfer
    usb_host_pause_transfers();
    
    // Handle specific requests that need special treatment
    if (pending_setup.bmRequestType == 0x00 && pending_setup.bRequest == 0x05) {
        // SET_ADDRESS - must handle locally with proper timing
        handleSetAddress();
        usb_host_resume_transfers();
        return;
    }
    
    // For all other requests, forward to device
    control_stage = CONTROL_SETUP;
    processControlTransfer();
}

// Handle SET_ADDRESS locally
void USBDeviceProxy::handleSetAddress() {
    uint8_t new_address = pending_setup.wValue & 0x7F;
    
    Serial4.print("S: SET_ADDRESS: ");
    Serial4.println(new_address);
    
    // Send ACK first (ZLP on IN endpoint)
    receiveData(NULL, 0);
    
    // Update device address (will take effect after status stage)
    USB1_DEVICEADDR = USB_DEVICEADDR_USBADR(new_address) | USB_DEVICEADDR_USBADRA;
    device_address = new_address;
    device_state = STATE_ADDRESS;
}

// Process control transfer state machine
void USBDeviceProxy::processControlTransfer() {
    if (control_stage != CONTROL_SETUP) {
        return;
    }
    
    // Forward the request to the connected device
    uint16_t actual_len = 0;
    bool success = false;
    
    if (pending_setup.bmRequestType & 0x80) {
        // Device-to-host (IN) transfer
        Serial4.println("I: Forwarding IN request to mouse...");
        success = usb_host_control_transfer(
            pending_setup.bmRequestType,
            pending_setup.bRequest,
            pending_setup.wValue,
            pending_setup.wIndex,
            pending_setup.wLength,
            proxy_descriptor_buffer,
            &actual_len,
            500  // 500ms timeout
        );
        
        if (success && actual_len > 0) {
            Serial4.print("I: Got ");
            Serial4.print(actual_len);
            Serial4.println(" bytes from mouse");
            
            // Debug: print first few bytes
            Serial4.print("I: Device descriptor: ");
            for (int i = 0; i < 8 && i < actual_len; i++) {
                if (proxy_descriptor_buffer[i] < 0x10) Serial4.print("0");
                Serial4.print(proxy_descriptor_buffer[i], HEX);
                Serial4.print(" ");
            }
            Serial4.println("...");
            
            // Send data to host
            setup_data_len = actual_len;
            sendData(proxy_descriptor_buffer, actual_len);
            control_stage = CONTROL_DATA_IN;
        } else if (success && actual_len == 0) {
            // No data, just send ZLP
            receiveData(NULL, 0);
            control_stage = CONTROL_IDLE;
            usb_host_resume_transfers();
        } else {
            // Failed - STALL
            Serial4.println("E: Control transfer to device failed!");
            USB1_ENDPTCTRL0 = 0x00010001;
            control_stage = CONTROL_IDLE;
            usb_host_resume_transfers();
        }
    } else {
        // Host-to-device (OUT) transfer
        if (pending_setup.wLength > 0) {
            // Need to receive data from host first
            Serial4.println("E: OUT transfers not implemented yet");
            USB1_ENDPTCTRL0 = 0x00010001;
            control_stage = CONTROL_IDLE;
            usb_host_resume_transfers();
        } else {
            // No data phase, forward immediately
            success = usb_host_control_transfer(
                pending_setup.bmRequestType,
                pending_setup.bRequest,
                pending_setup.wValue,
                pending_setup.wIndex,
                0,
                nullptr,
                &actual_len,
                500
            );
            
            if (success) {
                receiveData(NULL, 0);  // ACK
                control_stage = CONTROL_IDLE;
                usb_host_resume_transfers();
            } else {
                USB1_ENDPTCTRL0 = 0x00010001;  // STALL
                control_stage = CONTROL_IDLE;
                usb_host_resume_transfers();
            }
        }
    }
}

// Send data on endpoint 0 (following usb.c endpoint0_transmit pattern)
void USBDeviceProxy::sendData(const uint8_t* data, uint32_t length) {
    Serial4.print("I: Sending ");
    Serial4.print(length);
    Serial4.println(" bytes to host");
    
    if (length > 0) {
        // Setup data transfer - use OUR transfer descriptor
        proxy_endpoint0_transfer_data.next = 1;
        proxy_endpoint0_transfer_data.status = (length << 16) | (1<<7);
        uint32_t addr = (uint32_t)data;
        proxy_endpoint0_transfer_data.pointer0 = addr;
        proxy_endpoint0_transfer_data.pointer1 = addr + 4096;
        proxy_endpoint0_transfer_data.pointer2 = addr + 8192;
        proxy_endpoint0_transfer_data.pointer3 = addr + 12288;
        proxy_endpoint0_transfer_data.pointer4 = addr + 16384;
        
        // Queue data transfer - use OUR queue head
        proxy_endpoint_queue_head[1].next = (uint32_t)&proxy_endpoint0_transfer_data;
        proxy_endpoint_queue_head[1].status = 0;
        
        // Debug output before priming
        Serial4.print("I: QH[1] next = 0x");
        Serial4.println(proxy_endpoint_queue_head[1].next, HEX);
        Serial4.print("I: TD addr = 0x");
        Serial4.println((uint32_t)&proxy_endpoint0_transfer_data, HEX);
        Serial4.print("I: TD status = 0x");
        Serial4.println(proxy_endpoint0_transfer_data.status, HEX);
        
        USB1_ENDPTPRIME |= (1<<16);
        
        // Wait for ENDPTPRIME to clear with timeout
        uint32_t timeout = 1000;
        while ((USB1_ENDPTPRIME & (1<<16)) && timeout > 0) {
            timeout--;
            delayMicroseconds(1);
        }
        
        if (timeout == 0) {
            Serial4.println("E: Timeout waiting for ENDPTPRIME!");
            Serial4.println("E: TX endpoint failed to activate!");
            
            // Debug info
            Serial4.print("E: TD token = 0x");
            Serial4.println(proxy_endpoint0_transfer_data.status, HEX);
            Serial4.print("E: QH status = 0x");
            Serial4.println(proxy_endpoint_queue_head[1].status, HEX);
            Serial4.print("E: QH current = 0x");
            Serial4.println(proxy_endpoint_queue_head[1].current, HEX);
            return;
        }
    }
    
    // Setup ACK transfer (always needed for status stage) - use OUR transfer descriptor
    proxy_endpoint0_transfer_ack.next = 1;
    proxy_endpoint0_transfer_ack.status = (1<<7) | (1 << 15);  // Active + IOC
    proxy_endpoint0_transfer_ack.pointer0 = 0;
    proxy_endpoint_queue_head[0].next = (uint32_t)&proxy_endpoint0_transfer_ack;
    proxy_endpoint_queue_head[0].status = 0;
    
    // Clear complete flags
    USB1_ENDPTCOMPLETE = (1<<0) | (1<<16);
    
    // Prime OUT endpoint for ACK
    USB1_ENDPTPRIME |= (1<<0);
    proxy_endpoint0_notify_mask = (1 << 0);  // Notify on OUT complete
    endpoint0_notify_mask = proxy_endpoint0_notify_mask;
    while (USB1_ENDPTPRIME & (1<<0)) ;
}

// Send zero-length packet
void USBDeviceProxy::sendZLP() {
    Serial4.println("I: Sending ZLP");
    sendData(NULL, 0);
}

// Receive data on endpoint 0 (following usb.c endpoint0_receive pattern)
void USBDeviceProxy::receiveData(uint8_t* buffer, uint32_t length) {
    Serial4.print("I: Preparing to receive ");
    Serial4.print(length);
    Serial4.println(" bytes");
    
    if (length > 0) {
        // Setup data receive - use OUR transfer descriptor
        proxy_endpoint0_transfer_data.next = 1;
        proxy_endpoint0_transfer_data.status = (length << 16) | (1<<7);
        uint32_t addr = (uint32_t)buffer;
        proxy_endpoint0_transfer_data.pointer0 = addr;
        proxy_endpoint0_transfer_data.pointer1 = addr + 4096;
        proxy_endpoint0_transfer_data.pointer2 = addr + 8192;
        proxy_endpoint0_transfer_data.pointer3 = addr + 12288;
        proxy_endpoint0_transfer_data.pointer4 = addr + 16384;
        
        // Queue receive transfer - use OUR queue head
        proxy_endpoint_queue_head[0].next = (uint32_t)&proxy_endpoint0_transfer_data;
        proxy_endpoint_queue_head[0].status = 0;
        USB1_ENDPTPRIME |= (1<<0);
        while (USB1_ENDPTPRIME & (1<<0)) ;
    }
    
    // Setup ACK transfer - use OUR transfer descriptor
    proxy_endpoint0_transfer_ack.next = 1;
    proxy_endpoint0_transfer_ack.status = (1<<7) | (1 << 15);  // Active + IOC
    proxy_endpoint0_transfer_ack.pointer0 = 0;
    proxy_endpoint_queue_head[1].next = (uint32_t)&proxy_endpoint0_transfer_ack;
    proxy_endpoint_queue_head[1].status = 0;
    
    // Clear complete flags
    USB1_ENDPTCOMPLETE = (1<<0) | (1<<16);
    
    // Prime IN endpoint for ACK
    USB1_ENDPTPRIME |= (1<<16);
    proxy_endpoint0_notify_mask = (1 << 16);  // Notify on IN complete
    endpoint0_notify_mask = proxy_endpoint0_notify_mask;
    while (USB1_ENDPTPRIME & (1<<16)) ;
}

// Schedule transfer (following usb.c pattern)
void USBDeviceProxy::schedule_transfer(endpoint_t *endpoint, uint32_t mask, transfer_t *transfer) {
    // This is a simplified version - full implementation would handle queueing
    transfer->status |= (1<<15);  // Set IOC bit
    
    __disable_irq();
    endpoint->next = (uint32_t)transfer;
    endpoint->status = 0;
    USB1_ENDPTPRIME |= mask;
    endpoint->first_transfer = transfer;
    endpoint->last_transfer = transfer;
    __enable_irq();
}