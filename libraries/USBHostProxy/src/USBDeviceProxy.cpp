// USBDeviceProxy.cpp - USB Device Stack for Teensy 4.1 (Polling-based)
#include "USBDeviceProxy.h"
#include "USBHostDriver.h"
#include "SunBoxStartup.h"   // For debug state checking
#include "SunBoxLogger.h"    // For logger
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

// Buffers for larger transfers
static uint8_t proxy_descriptor_buffer[512] __attribute__((aligned(32)));

// Buffer for data endpoint transfers
static uint8_t proxy_in_buffers[NUM_ENDPOINTS][64] __attribute__((aligned(32)));
static transfer_t proxy_in_transfers[NUM_ENDPOINTS] __attribute__((aligned(32)));

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
    endpointN_notify_mask(0),
    pending_has_data(false),
    num_endpoints(0),
    hostDriver(nullptr),
    device_high_speed(true) {  // Default to high speed
    
    memset(&pending_setup, 0, sizeof(pending_setup));
    memset(&pending_setup_saved, 0, sizeof(pending_setup_saved));
    memset(setup_data_buffer, 0, sizeof(setup_data_buffer));
    memset(endpoints, 0, sizeof(endpoints));
    memset(endpoint_ready, 0, sizeof(endpoint_ready));

    // Initialize descriptor cache
    memset(&desc_cache, 0, sizeof(desc_cache));
    desc_cache.invalidate();
}

// Get the actual device speed from host driver
uint8_t USBDeviceProxy::getActualDeviceSpeed() const {
    if (!hostDriver) return 2; // Default to high speed if no driver
    return ((USBHostDriver*)hostDriver)->getDeviceSpeed();
}

// Initialize USB hardware - following usb_init() exactly
void USBDeviceProxy::begin() {
    LOG_STARTUP(LOG_BOOT, "Initializing USB Host Hardware...");
    
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
        
        LOG_WARNING(LOG_BOOT, "PHY needs reset");
        
        // Reset PHY
        USBPHY1_CTRL_SET = USBPHY_CTRL_SFTRST;
        
        // Reset controller
        USB1_USBCMD |= USB_USBCMD_RST;
        int count = 0;
        while (USB1_USBCMD & USB_USBCMD_RST) {
            count++;
        }
        LOG_DEBUGF(LOG_BOOT, "USB reset took %d loops", count);
        
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
    
    // NEW: Configure USB speed based on detected device speed
    if (!device_high_speed) {
        // Force Full Speed (12 Mbps) operation
        USBPHY1_CTRL_SET = USBPHY_CTRL_ENUTMILEVEL2 | USBPHY_CTRL_ENUTMILEVEL3;
        USB1_PORTSC1 |= USB_PORTSC1_PFSC;
        LOG_STARTUP(LOG_BOOT, "USB Host configured for Full Speed (12 Mbps) based on physical device");
    } else {
        // Allow High Speed (480 Mbps) operation (default)
        USBPHY1_CTRL_CLR = USBPHY_CTRL_ENUTMILEVEL2 | USBPHY_CTRL_ENUTMILEVEL3;
        USB1_PORTSC1 &= ~USB_PORTSC1_PFSC;
        LOG_STARTUP(LOG_BOOT, "USB Host configured for High Speed (480 Mbps) based on physical device");
    }
    
    // 7. Set device mode (from usb.c line 613)
    USB1_USBMODE = USB_USBMODE_CM(2) | USB_USBMODE_SLOM;
    
    // 8. Initialize endpoint queue heads (from usb.c line 615-620)
    memset(proxy_endpoint_queue_head, 0, sizeof(proxy_endpoint_queue_head));
    
    // Dynamic EP0 configuration based on actual device
    uint16_t ep0_max_size = 64;  // Default
    
    if (hostDriver && hostDriver->isReady()) {
        // Get the EP0 size directly from the physical device's descriptor
        // This is the CRITICAL FIX - use actual bMaxPacketSize0 instead of inferring from speed
        ep0_max_size = hostDriver->getDeviceEP0Size();
        
        LOG_DEBUGF(LOG_BOOT, "Configuring USB Device EP0 with size from physical device: %d bytes", ep0_max_size);
    } else {
        LOG_WARNING(LOG_BOOT, "No host driver ready, using default EP0 size of 64 bytes");
    }
    
    proxy_endpoint_queue_head[0].config = (ep0_max_size << 16) | (1 << 15);  // EP0 OUT
    proxy_endpoint_queue_head[1].config = (ep0_max_size << 16);              // EP0 IN
    
    // 9. Set endpoint list address - CRITICAL: Use OUR queue heads!
    USB1_ENDPOINTLISTADDR = (uint32_t)&proxy_endpoint_queue_head;
    
    // DEBUG: Verify the address
    LOG_DEBUGF(LOG_BOOT, "proxy_endpoint_queue_head address: 0x%08X", (uint32_t)proxy_endpoint_queue_head);
    LOG_DEBUGF(LOG_BOOT, "USB1_ENDPOINTLISTADDR set to: 0x%08X", USB1_ENDPOINTLISTADDR);
    
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
    
    LOG_STARTUP(LOG_BOOT, "USB Host Hardware Initialized");
}

// Main polling function - MUST be called frequently from loop()
void USBDeviceProxy::poll() {
    // Track polling rate for debugging
    uint32_t now = micros();
    if (last_poll_time != 0) {
        uint32_t delta = now - last_poll_time;
        if (delta > 100) {  // More than 100us since last poll
            if ((poll_count % 10000) == 0) {
                LOG_WARNINGF(LOG_DATA, "Slow polling detected: %lus", delta);
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
            logger.error("USBDeviceProxy::poll() - USB Error detected!");
        }
    }
    
    // Always check for setup packets (high priority)
    pollControlEndpoint();
    
    // Process any pending control transfers
    processControlTransfer();
    
    // Poll data endpoints if configured
    if (device_state == STATE_CONFIGURED) {
        pollDataEndpoints();
    }
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
    
    // Check for endpoint0 completions
    uint32_t completestatus = USB1_ENDPTCOMPLETE;
    if (completestatus & ((1<<0) | (1<<16))) { // Endpoint 0 IN or OUT
        // Clear the endpoint0 completion flags
        USB1_ENDPTCOMPLETE = (1<<0) | (1<<16);
        
        // Handle completion based on the current state
        if (control_stage == CONTROL_DATA_OUT) {
            // Data OUT stage completed (e.g., for SET_REPORT)
            // The data should now be in the buffer
            LOG_DEBUG(LOG_ENUM, "EP0 OUT data received");
            
            // Now process the control transfer
            processControlTransfer();
        }
        else if (control_stage == CONTROL_DATA_IN && (completestatus & (1<<16))) {
            LOG_DEBUG(LOG_ENUM, "EP0 TX complete");
            control_stage = CONTROL_STATUS_OUT;
            // Prepare to receive status
            receiveData(NULL, 0);
        }
        else if (control_stage == CONTROL_STATUS_OUT) {
            LOG_DEBUG(LOG_ENUM, "Control transfer complete!");
            control_stage = CONTROL_IDLE;
        }
    }
}

// Poll data endpoints for IN token from host
void USBDeviceProxy::pollDataEndpoints() {
    // Check for any data endpoint completions
    uint32_t completestatus = USB1_ENDPTCOMPLETE;
    uint32_t data_ep_mask = 0xFFFEFFFE;  // All endpoints except EP0 (clear bits 0 and 16)
    uint32_t data_completions = completestatus & data_ep_mask;
    
    if (data_completions) {
        // Clear only the data endpoint flags we're processing
        USB1_ENDPTCOMPLETE = data_completions;
        
        // Debug log for data endpoint completions
        static uint32_t total_completions = 0;
        total_completions++;
        if ((total_completions % 100) == 1) {
            LOG_DEBUGF(LOG_ENUM, "Data EP complete 0x%X (#%lu)", data_completions, total_completions);
        }
    }
    
    // For each configured endpoint, check if it completed
    for (uint8_t i = 0; i < num_endpoints; i++) {
        if (!endpoints[i].configured) continue;
        
        uint8_t ep_num = endpoints[i].address & 0x0F;
        uint8_t ep_dir = (endpoints[i].address & 0x80) ? 1 : 0;
        
        if (ep_dir == 0) continue; // Only handle IN endpoints for now
        
        // Check if this endpoint completed
        uint32_t mask = 1 << (ep_num + 16); // IN endpoint mask
        if (data_completions & mask) {
            // The host has consumed our data, we can send more
            endpoint_ready[i] = true;
            
            // Debug log for specific endpoint
            static uint32_t ep_completions[8] = {0};
            ep_completions[ep_num]++;
            if ((ep_completions[ep_num] % 100) == 1) {
                LOG_DEBUGF(LOG_ENUM, "EP%d ready again (#%lu)", ep_num, ep_completions[ep_num]);
            }
        }
    }
}

// Handle USB interrupt status (called from poll)
void USBDeviceProxy::handleUSBInterrupt() {
    // Check endpoint complete status
    uint32_t completestatus = USB1_ENDPTCOMPLETE;
    if (completestatus) {
        // Only clear endpoint 0 flags here - let pollDataEndpoints handle data endpoints
        uint32_t ep0_mask = 0x00010001;  // Bits 0 and 16 for EP0 OUT and IN
        uint32_t ep0_status = completestatus & ep0_mask;
        
        if (ep0_status) {
            USB1_ENDPTCOMPLETE = ep0_status;  // Only clear EP0 flags
            
            // Debug output for EP0
            LOG_DEBUGF(LOG_ENUM, "ENDPTCOMPLETE EP0 = 0x%X", ep0_status);
        }
        
        // Handle endpoint 0 completions
        if (ep0_status & (1<<0)) {  // EP0 OUT completion (for SET_REPORT data)
            if (control_stage == CONTROL_DATA_OUT) {
                LOG_DEBUG(LOG_ENUM, "EP0 OUT data phase complete");
                // Don't call processControlTransfer here - let pollControlEndpoint handle it
            }
        }
        
        if (ep0_status & (1<<16)) {  // EP0 IN completion
            if (control_stage == CONTROL_DATA_IN) {
                LOG_DEBUG(LOG_ENUM, "EP0 IN data phase complete");
                control_stage = CONTROL_STATUS_OUT;
                // Prepare to receive status
                receiveData(NULL, 0);
            }
        }
        
        if (ep0_status & endpoint0_notify_mask) {
            proxy_endpoint0_notify_mask = 0;
            endpoint0_notify_mask = 0;
            // Handle completion based on control stage
            if (control_stage == CONTROL_STATUS_OUT) {
                LOG_DEBUG(LOG_ENUM, "Control transfer complete!");
                control_stage = CONTROL_IDLE;
            }
        }

        // Don't process other endpoints here - let pollDataEndpoints handle them
    }
}

// Handle USB reset
void USBDeviceProxy::handleUSBReset() {
    LOG_WARNING(LOG_CONNECT, "USB Reset detected");
    
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
    
    // Reset endpoint ready states
    memset(endpoint_ready, 0, sizeof(endpoint_ready));
    
    // If we were in the middle of a control transfer, clean up
    if (control_stage != CONTROL_IDLE) {
        control_stage = CONTROL_IDLE;
    }

    // Invalidate descriptor cache on bus reset ONLY if device identity changed
    if (hostDriver) {
        uint16_t vid = hostDriver->getVendorID();
        uint16_t pid = hostDriver->getProductID();
        if (!desc_cache.identity_valid ||
            desc_cache.cached_vid != vid ||
            desc_cache.cached_pid != pid) {
            desc_cache.invalidate();
        } else {
            // Cache retained — same device on bus reset
        }
    }
}

// Handle port change
void USBDeviceProxy::handlePortChange() {
    // Check negotiated speed
    if (USB1_PORTSC1 & USB_PORTSC1_HSP) {
        LOG_DEBUG(LOG_CONNECT, "High-speed device operation detected");
        proxy_usb_high_speed = 1;

        // Verify this matches our configuration
        if (!device_high_speed) {
            LOG_WARNING(LOG_CONNECT, "Speed mismatch - configured for Full Speed but negotiated High Speed!");
            LOG_WARNING(LOG_CONNECT, "Host may not support Full Speed on this port, proxying could malfunction & device emulation may fail.");
        }
    } else {
        LOG_DEBUG(LOG_CONNECT, "Full-speed device operation detected");
        proxy_usb_high_speed = 0;

        // Verify this matches our configuration
        if (device_high_speed) {
            LOG_WARNING(LOG_CONNECT, "Successfully forced Full Speed operation despite High Speed capability");
        }
    }
}

void USBDeviceProxy::handleSetupPacket(uint32_t setup0, uint32_t setup1) {
    // logger.debugf("SETUP: bmRequestType=0x%02X bRequest=0x%02X wValue=0x%04X wIndex=0x%04X wLength=%d",
    //               pending_setup.bmRequestType, pending_setup.bRequest, 
    //               pending_setup.wValue, pending_setup.wIndex, pending_setup.wLength);
    
    // // Log control transfer stage transition
    // logger.debugf("Control stage transition: %s -> SETUP", 
    //               control_stage == CONTROL_IDLE ? "IDLE" : 
    //               control_stage == CONTROL_DATA_IN ? "DATA_IN" : 
    //               control_stage == CONTROL_DATA_OUT ? "DATA_OUT" : "UNKNOWN");
    control_stage = CONTROL_IDLE;  // Reset to idle for new setup
    
    // Check if we have a connected device to proxy
    if (!hostDriver || !hostDriver->isReady()) {
        logger.error("No USB device connected to proxy!");
        USB1_ENDPTCTRL0 = 0x00010001;  // STALL both directions
        return;
    }

    // ============================ NEW REQUEST FILTERING LOGIC ============================
    // Some devices (like Logitech) STALL on standard-but-optional requests.
    // Instead of forwarding and recovering from the stall, we intercept and handle them here.

    // Filter 1: GET_DESCRIPTOR for Device Qualifier (wValue = 0x0600)
    // Full-speed devices are not required to have this. We can just ACK (stall).
    if (pending_setup.bmRequestType == 0x80 && pending_setup.bRequest == 0x06 && pending_setup.wValue == 0x0600) {
        LOG_DEBUG(LOG_ENUM, "Filtering GET_DESCRIPTOR(DeviceQualifier) request. Stalling.");
        USB1_ENDPTCTRL0 = 0x00010001; // STALL both directions
        control_stage = CONTROL_IDLE;
        return;
    }

    // Filter 2: SET_IDLE (bRequest = 0x0A)
    // This HID class request is also optional and causes stalls on some devices.
    if (pending_setup.bmRequestType == 0x21 && pending_setup.bRequest == 0x0A) {
        LOG_DEBUG(LOG_ENUM, "Filtering SET_IDLE request. Acknowledging and ignoring.");
        receiveData(NULL, 0); // Send ZLP to acknowledge
        control_stage = CONTROL_IDLE;
        return;
    }
    // ========================== END OF NEW REQUEST FILTERING LOGIC =========================

    // Handle specific requests that need special treatment
    if (pending_setup.bmRequestType == 0x00 && pending_setup.bRequest == 0x05) {
        // SET_ADDRESS - must handle locally with proper timing
        handleSetAddress();
        return;
    }
    
    // Handle CLEAR_FEATURE for endpoints locally
    if (pending_setup.bRequest == 0x01 && // CLEAR_FEATURE
        pending_setup.bmRequestType == 0x02 && // Endpoint recipient
        (pending_setup.wIndex & 0x0F) != 0) { // Not endpoint 0
        LOG_DEBUG(LOG_ENUM, "Handling CLEAR_FEATURE locally for endpoint");
        handleClearFeature();
        return;
    }
    
    // Handle SET_CONFIGURATION specially - need to configure endpoints
    if (pending_setup.bmRequestType == 0x00 && pending_setup.bRequest == 0x09) {
        // Note: SET_CONFIGURATION needs special handling but we'll keep data flowing
        // Only pause if absolutely necessary for reconfiguration
        hostDriver->pauseDataTransfers();
        
        // Forward to device first
        LOG_DEBUG(LOG_ENUM, "Forwarding SET_CONFIGURATION to device...");
        uint16_t dummy_len;
        bool success = hostDriver->controlTransfer(
            pending_setup.bmRequestType,
            pending_setup.bRequest,
            pending_setup.wValue,
            pending_setup.wIndex,
            0,
            nullptr,
            &dummy_len,
            500
        );

        if (success) {
            LOG_DEBUG(LOG_ENUM, "SET_CONFIGURATION forwarded successfully");
            configuration_value = pending_setup.wValue;
            proxy_usb_configuration = configuration_value;
            
            // Parse descriptors and configure endpoints
            if (configuration_value > 0) {
                parseConfigurationDescriptor();
                configureAllEndpoints();
                device_state = STATE_CONFIGURED;
            }
            
            // ACK after endpoint configuration is complete
            receiveData(NULL, 0);
            control_stage = CONTROL_IDLE;
        } else {
            USB1_ENDPTCTRL0 = 0x00010001;  // STALL
            control_stage = CONTROL_IDLE;
        }
        
        // Resume after configuration changes
        hostDriver->resumeDataTransfers();
        return;
    }

    // Handle HID Class SET_REPORT request specially
    if (pending_setup.bmRequestType == 0x21 && pending_setup.bRequest == 0x09) {
        // This is a SET_REPORT request (Host->Device, Class, Interface)
#if SUNBOX_LOGGING
        uint16_t report_type = (pending_setup.wValue >> 8);
        uint16_t report_id = (pending_setup.wValue & 0xFF);
        uint16_t interface = pending_setup.wIndex;
        LOG_DEBUGF(LOG_ENUM, "SET_REPORT for interface %d, type=%d, ID=%d, length=%d",
                      interface, report_type, report_id, pending_setup.wLength);
#endif
        
        // Store setup packet for later forwarding
        memcpy(&pending_setup_saved, &pending_setup, sizeof(setup_packet_t));
        
        // We need to receive data from host first, then forward to device
        if (pending_setup.wLength > 0) {
            LOG_DEBUG(LOG_ENUM, "Receiving SET_REPORT data from host...");
            
            // Receive data from host
            receiveData(setup_data_buffer, pending_setup.wLength);
            pending_has_data = true;
            control_stage = CONTROL_DATA_OUT;
            
            // Setup data will be forwarded in processControlTransfer after receiving
            return;
        }
    }
    
    // Handle GET_DESCRIPTOR for strings specially to ensure proper forwarding
    if (pending_setup.bmRequestType == 0x80 && pending_setup.bRequest == 0x06) {
        uint8_t desc_type = (pending_setup.wValue >> 8) & 0xFF;
#if SUNBOX_LOGGING
        uint8_t desc_index = pending_setup.wValue & 0xFF;
#endif

        if (desc_type == 0x03) { // String descriptor
            LOG_DEBUGF(LOG_ENUM, "GET_STRING_DESCRIPTOR index=%d langID=0x%04X", desc_index, pending_setup.wIndex);
            
            // For string descriptors, we need to handle the language ID properly
            control_stage = CONTROL_SETUP;
            processControlTransfer();
            return;
        }
    }
    
    // For all other requests, forward to device WITHOUT pausing
    control_stage = CONTROL_SETUP;
    processControlTransfer();
}

// Handle SET_ADDRESS locally
void USBDeviceProxy::handleSetAddress() {
    uint8_t new_address = pending_setup.wValue & 0x7F;
    
    LOG_DEBUGF(LOG_ENUM, "SET_ADDRESS: %d", new_address);
    
    // Send ACK first (ZLP on IN endpoint)
    receiveData(NULL, 0);
    
    // Update device address (will take effect after status stage)
    USB1_DEVICEADDR = USB_DEVICEADDR_USBADR(new_address) | USB_DEVICEADDR_USBADRA;
    device_address = new_address;
    device_state = STATE_ADDRESS;
}

// =============================================================================
// Descriptor Cache Implementation
// =============================================================================

void USBDeviceProxy::invalidateDescriptorCache() {
    desc_cache.invalidate();
}

// Check if this is a cacheable GET_DESCRIPTOR and try to serve from cache.
// Returns true if the response was served from cache (caller should return).
bool USBDeviceProxy::tryServeCachedDescriptor() {
    // Only cache standard GET_DESCRIPTOR: bmRequestType == 0x80, bRequest == 0x06
    if (pending_setup.bmRequestType != 0x80 || pending_setup.bRequest != 0x06) {
        return false;
    }

    uint8_t desc_type  = (pending_setup.wValue >> 8) & 0xFF;
    uint8_t desc_index = pending_setup.wValue & 0xFF;

    // Only cache device(0x01), config(0x02), string(0x03), BOS(0x0F)
    if (desc_type != 0x01 && desc_type != 0x02 &&
        desc_type != 0x03 && desc_type != 0x0F) {
        return false;
    }

    // Validate device identity before serving cached data
    if (desc_cache.identity_valid && hostDriver) {
        uint16_t vid = hostDriver->getVendorID();
        uint16_t pid = hostDriver->getProductID();
        if (vid != desc_cache.cached_vid || pid != desc_cache.cached_pid) {
            desc_cache.invalidate();
            return false;
        }
    }

    uint16_t cached_len = 0;
    const uint8_t* cached_data = desc_cache.lookup(desc_type, desc_index,
                                                    pending_setup.wIndex, &cached_len);
    if (!cached_data || cached_len == 0) {
        return false;
    }

    // Cache HIT — serve min(cached_length, wLength)
    uint16_t serve_len = (cached_len < pending_setup.wLength) ? cached_len : pending_setup.wLength;

    // Copy cached data into proxy_descriptor_buffer so sendData() can DMA it
    memcpy(proxy_descriptor_buffer, cached_data, serve_len);

    setup_data_len = serve_len;
    sendData(proxy_descriptor_buffer, serve_len);
    control_stage = CONTROL_DATA_IN;
    return true;
}

// Store a descriptor response in cache after successful fetch.
// Applies the config/BOS two-phase rule: only cache full fetches.
void USBDeviceProxy::storeDescriptorInCache(uint8_t desc_type, uint8_t desc_index,
                                             uint16_t wIndex, const uint8_t* data,
                                             uint16_t len) {
    if (len == 0) return;

    // Update device identity if we have a host driver
    if (hostDriver && !desc_cache.identity_valid) {
        desc_cache.cached_vid = hostDriver->getVendorID();
        desc_cache.cached_pid = hostDriver->getProductID();
        desc_cache.identity_valid = true;
    }

    switch (desc_type) {
        case 0x01: // Device descriptor — always cache
            desc_cache.store(desc_type, desc_index, wIndex, data, len);
            break;

        case 0x02: { // Configuration descriptor — only cache full fetch
            if (len >= 4) {
                uint16_t wTotalLength = data[2] | (data[3] << 8);
                if (len >= wTotalLength) {
                    desc_cache.store(desc_type, desc_index, wIndex, data, len);
                }
            }
            break;
        }

        case 0x03: // String descriptor — always cache
            desc_cache.store(desc_type, desc_index, wIndex, data, len);
            break;

        case 0x0F: { // BOS descriptor — only cache full fetch
            if (len >= 4) {
                uint16_t wTotalLength = data[2] | (data[3] << 8);
                if (len >= wTotalLength) {
                    desc_cache.store(desc_type, desc_index, wIndex, data, len);
                }
            }
            break;
        }

        default:
            break;
    }
}

// Process control transfer state machine
void USBDeviceProxy::processControlTransfer() {
    if (control_stage == CONTROL_IDLE) {
        return;
    }
    
    // Handle data-out phase completion for SET_REPORT
    if (control_stage == CONTROL_DATA_OUT && pending_has_data) {
        // We've received data for a SET_REPORT request
        LOG_DEBUG(LOG_ENUM, "SET_REPORT data received from host");

        // Debug: print first few bytes
        if (logger.isChannelEnabled(LOG_ENUM)) {
            String dataStr = "Data: ";
            for (int i = 0; i < 16 && i < pending_setup_saved.wLength; i++) {
                if (setup_data_buffer[i] < 0x10) dataStr += "0";
                dataStr += String(setup_data_buffer[i], HEX);
                dataStr += " ";
            }
            if (pending_setup_saved.wLength > 16) dataStr += "...";
            logger.debug(dataStr.c_str());
        }
        
        // Forward the SET_REPORT with data to the device (no pause needed)
        uint16_t actual_len = 0;
        bool success = false;
        
        if (pending_setup_saved.wLength > 0) {
            success = hostDriver->controlTransfer(
                pending_setup_saved.bmRequestType,
                pending_setup_saved.bRequest,
                pending_setup_saved.wValue,
                pending_setup_saved.wIndex,
                pending_setup_saved.wLength,
                setup_data_buffer,  // This contains the data from the host
                &actual_len,
                500  // 500ms timeout
            );
        } else {
            success = hostDriver->controlTransfer(
                pending_setup_saved.bmRequestType,
                pending_setup_saved.bRequest,
                pending_setup_saved.wValue,
                pending_setup_saved.wIndex,
                0,
                nullptr,
                &actual_len,
                500
            );
        }
        
        if (success) {
            // Successfully forwarded to device, acknowledge to host
            LOG_DEBUG(LOG_ENUM, "SET_REPORT forwarded to device successfully");
            
            // ACK to host (ZLP for successful control transfer)
            sendZLP();
            control_stage = CONTROL_IDLE;
        } else {
            // Failed to forward to device
            LOG_DEBUG(LOG_ENUM, "Failed to forward SET_REPORT to device");
            USB1_ENDPTCTRL0 = 0x00010001;  // STALL
            control_stage = CONTROL_IDLE;
        }
        
        pending_has_data = false;
        return;
    }
    
    if (control_stage != CONTROL_SETUP) {
        return;
    }
    
    // Forward the request to the connected device
    uint16_t actual_len = 0;
    bool success = false;
    
    if (pending_setup.bmRequestType & 0x80) {
        // Device-to-host (IN) transfer

        // --- Descriptor cache lookup (before hitting the real device) ---
        if (tryServeCachedDescriptor()) {
            return;  // Served from cache, no round-trip needed
        }

        success = hostDriver->controlTransfer(
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
            LOG_DEBUGF(LOG_ENUM, "Got %d bytes from mouse", actual_len);

            // Debug: print first few bytes
            if (logger.isChannelEnabled(LOG_ENUM)) {
                String dataStr = "Data: ";
                for (int i = 0; i < 8 && i < actual_len; i++) {
                    if (proxy_descriptor_buffer[i] < 0x10) dataStr += "0";
                    dataStr += String(proxy_descriptor_buffer[i], HEX);
                    dataStr += " ";
                }
                dataStr += "...";
                logger.debug(dataStr.c_str());
            }

            // CRITICAL FIX: For string descriptors, send ONLY the actual descriptor length
            // This trimming MUST happen BEFORE caching so the cached value is correct.
            uint8_t desc_type = (pending_setup.wValue >> 8) & 0xFF;
            if (pending_setup.bRequest == 0x06 && desc_type == 0x03) {
                // String descriptor - first byte contains the actual length
                if (actual_len > 0 && proxy_descriptor_buffer[0] <= actual_len) {
                    actual_len = proxy_descriptor_buffer[0]; // Use the descriptor's reported length
                    LOG_DEBUGF(LOG_ENUM, "String descriptor actual length: %d", actual_len);
                }
            }

            // --- Store in descriptor cache (after trimming, before sending) ---
            if (pending_setup.bRequest == 0x06 && pending_setup.bmRequestType == 0x80) {
                uint8_t cache_desc_type  = (pending_setup.wValue >> 8) & 0xFF;
                uint8_t cache_desc_index = pending_setup.wValue & 0xFF;
                storeDescriptorInCache(cache_desc_type, cache_desc_index,
                                       pending_setup.wIndex,
                                       proxy_descriptor_buffer, actual_len);
            }

            // Special handling for HID GET_REPORT responses
            if (pending_setup.bmRequestType == 0xA1 && pending_setup.bRequest == 0x01) {
                LOG_DEBUG(LOG_ENUM, "Processing HID GET_REPORT response");
                // First byte in proxy_descriptor_buffer might need to be preserved
                // This is device-specific handling if needed
            }

            // Send data to host
            setup_data_len = actual_len;
            sendData(proxy_descriptor_buffer, actual_len);
            control_stage = CONTROL_DATA_IN;
        } else if (success && actual_len == 0) {
            // No data, just send ZLP
            receiveData(NULL, 0);
            control_stage = CONTROL_IDLE;
        } else {
            // Failed - STALL
            LOG_DEBUG(LOG_ENUM, "Control transfer to device failed!");
            LOG_DEBUG(LOG_ENUM, "Sending STALL to host (ENDPTCTRL0 = 0x00010001)");
            USB1_ENDPTCTRL0 = 0x00010001;
            control_stage = CONTROL_IDLE;

            // Log more details about the failed request
            LOG_DEBUGF(LOG_ENUM, "Failed request was: Type=0x%02X Req=0x%02X Val=0x%04X Idx=0x%04X Len=%d",
                         pending_setup.bmRequestType, pending_setup.bRequest,
                         pending_setup.wValue, pending_setup.wIndex, pending_setup.wLength);
        }
    } else {
        // Host-to-device (OUT) transfer
        if (pending_setup.wLength > 0) {
            // For SET_REPORT, this should be handled above after data is received
            // For other OUT transfers, they're not supported yet
            LOG_DEBUG(LOG_ENUM, "OUT transfers not implemented yet");
            USB1_ENDPTCTRL0 = 0x00010001;
            control_stage = CONTROL_IDLE;
        } else {
            // No data phase, forward immediately
            success = hostDriver->controlTransfer(
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
            } else {
                USB1_ENDPTCTRL0 = 0x00010001;  // STALL
                control_stage = CONTROL_IDLE;
            }
        }
    }
}

// Send data on endpoint 0 (following usb.c endpoint0_transmit pattern)
void USBDeviceProxy::sendData(const uint8_t* data, uint32_t length) {
    LOG_DEBUGF(LOG_ENUM, "Sending %lu bytes to host", length);
    
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
        LOG_DEBUGF(LOG_ENUM, "QH[1] next = 0x%08X", proxy_endpoint_queue_head[1].next);
        LOG_DEBUGF(LOG_ENUM, "TD addr = 0x%08X", (uint32_t)&proxy_endpoint0_transfer_data);
        LOG_DEBUGF(LOG_ENUM, "TD status = 0x%08X", proxy_endpoint0_transfer_data.status);
        
        USB1_ENDPTPRIME |= (1<<16);
        
        // Wait for ENDPTPRIME to clear with timeout
        uint32_t timeout = 1000;
        while ((USB1_ENDPTPRIME & (1<<16)) && timeout > 0) {
            timeout--;
            delayMicroseconds(1);
        }
        
        if (timeout == 0) {
            logger.warning("Timeout waiting for ENDPTPRIME!");
            logger.warning("TX endpoint failed to activate!");
            
            // Debug info
            logger.warningf("TD token = 0x%08X", proxy_endpoint0_transfer_data.status);
            logger.warningf("QH status = 0x%08X", proxy_endpoint_queue_head[1].status);
            logger.warningf("QH current = 0x%08X", proxy_endpoint_queue_head[1].current);
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
    LOG_DEBUG(LOG_ENUM, "Sending ZLP");
    sendData(NULL, 0);
}

// Receive data on endpoint 0 (following usb.c endpoint0_receive pattern)
void USBDeviceProxy::receiveData(uint8_t* buffer, uint32_t length) {
    LOG_DEBUGF(LOG_ENUM, "Preparing to receive %lu bytes", length);
    
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

// Parse configuration descriptor to find endpoints
void USBDeviceProxy::parseConfigurationDescriptor() {
    LOG_DEBUG(LOG_ENUM, "Parsing configuration descriptor for endpoints...");
    
    // We already forwarded the configuration descriptor earlier, so we should have it cached
    // Let's request it again to ensure we have the full descriptor
    uint16_t config_len = 0;
    uint8_t config_desc[512];
    
    // Request configuration descriptor
    bool success = hostDriver->controlTransfer(
        0x80,  // bmRequestType
        0x06,  // GET_DESCRIPTOR
        0x0200,  // Configuration descriptor
        0,     // Index
        sizeof(config_desc),
        config_desc,
        &config_len,
        500
    );
    
    if (!success || config_len < 9) {
        logger.error("Failed to get configuration descriptor!");
        return;
    }
    
    LOG_DEBUGF(LOG_ENUM, "Got configuration descriptor, length: %d", config_len);
    
    // Get actual total length from descriptor
    uint16_t total_len = config_desc[2] | (config_desc[3] << 8);
    LOG_DEBUGF(LOG_ENUM, "Total configuration length: %d", total_len);
    
    // If we didn't get the full descriptor, request it again
    if (config_len < total_len) {
        success = hostDriver->controlTransfer(
            0x80,  // bmRequestType
            0x06,  // GET_DESCRIPTOR
            0x0200,  // Configuration descriptor
            0,     // Index
            total_len,
            config_desc,
            &config_len,
            500
        );
        
        if (!success) {
            logger.error("Failed to get full configuration descriptor!");
            return;
        }
    }
    
    // Reset endpoint count
    num_endpoints = 0;
    
    // Parse the descriptor
    uint16_t offset = 0;
    while (offset < config_len && num_endpoints < MAX_PROXY_ENDPOINTS) {
        if (offset + 2 > config_len) break; // Need at least 2 bytes for length and type
        
        uint8_t desc_len = config_desc[offset];
        uint8_t desc_type = config_desc[offset + 1];
        
        if (desc_len == 0 || offset + desc_len > config_len) break;
        
        // Look for endpoint descriptors
        if (desc_type == 0x05 && desc_len >= 7) {  // Endpoint descriptor
            uint8_t ep_addr = config_desc[offset + 2];
            uint8_t ep_attr = config_desc[offset + 3];
            uint16_t max_packet = config_desc[offset + 4] | (config_desc[offset + 5] << 8);
            uint8_t interval = config_desc[offset + 6];
            
            LOG_DEBUGF(LOG_ENUM, "Found endpoint 0x%02X type=%d size=%d interval=%d",
                         ep_addr, ep_attr & 0x03, max_packet, interval);
            
            // Store endpoint info
            endpoints[num_endpoints].address = ep_addr;
            endpoints[num_endpoints].attributes = ep_attr;
            endpoints[num_endpoints].maxPacketSize = max_packet;
            endpoints[num_endpoints].interval = interval;
            endpoints[num_endpoints].configured = false;
            endpoint_ready[num_endpoints] = false;
            num_endpoints++;
        }
        
        offset += desc_len;
    }
    
    LOG_DEBUGF(LOG_ENUM, "Found %d endpoints to configure", num_endpoints);
}

// Configure a single endpoint, replicating the official Teensy core library logic.
void USBDeviceProxy::configureEndpoint(uint8_t addr, uint8_t type, uint16_t maxPacket, uint8_t interval) {
    uint8_t ep_num = addr & 0x0F;
    uint8_t ep_dir = (addr & 0x80) ? 1 : 0;

    if (ep_num == 0 || ep_num > 7) {
        LOG_DEBUGF(LOG_ENUM, "Invalid endpoint number: %d", ep_num);
        return;
    }

    LOG_DEBUGF(LOG_ENUM, "Configuring endpoint %d %s type=%d size=%d interval=%d",
                 ep_num, ep_dir ? "IN" : "OUT", type & 0x03, maxPacket, interval);

    // Special handling for Low Speed devices
    if (hostDriver && hostDriver->getDeviceSpeed() == 0) {
        LOG_DEBUG(LOG_ENUM, "Low Speed device - adjusting endpoint configuration");
        // Low Speed devices typically have smaller endpoints
        // BenQ ZOWIE has 8-byte endpoints
    }

    // ======================== THE DEFINITIVE FIX ========================
    // Based on the official usb.c, the queue head configuration for a
    // standard interrupt/bulk transmit endpoint must include bit 29 (ZLT).
    // This prevents the hardware from prematurely halting the transfer queue.
    // This is the key piece of hardware configuration that was missing.
    //
    uint32_t config = (maxPacket << 16) | (1 << 29);
    // ====================== END OF DEFINITIVE FIX =======================

    // We do NOT set MULT or the 'I' bit for a standard interrupt endpoint.
    // The official code confirms this. The ZLT bit is the only one needed.

    proxy_endpoint_queue_head[ep_num * 2 + ep_dir].config = config;
    proxy_endpoint_queue_head[ep_num * 2 + ep_dir].current = 0;
    proxy_endpoint_queue_head[ep_num * 2 + ep_dir].next = 1;  // Terminate list
    proxy_endpoint_queue_head[ep_num * 2 + ep_dir].status = 0;

    // Configure the endpoint control register (this part was already correct)
    volatile uint32_t* endptctrl = &USB1_ENDPTCTRL0 + ep_num;
    uint32_t ctrl = *endptctrl;

    if (ep_dir) {
        // TX (IN) endpoint
        ctrl &= 0x0000FFFF;
        ctrl |= (1 << 23);   // TXE - TX Enable
        ctrl |= (1 << 22);   // TXR - TX Data Toggle Reset
        ctrl |= ((type & 0x03) << 18);  // TXT - TX Endpoint Type (Interrupt)
    } else {
        // RX (OUT) endpoint
        ctrl &= 0xFFFF0000;
        ctrl |= (1 << 7);    // RXE - RX Enable
        ctrl |= (1 << 6);    // RXR - RX Data Toggle Reset
        ctrl |= ((type & 0x03) << 2);   // RXT - RX Endpoint Type
    }

    *endptctrl = ctrl;

    // Enable notifications for this endpoint
    if (ep_dir) {
        proxy_endpointN_notify_mask |= (1 << (ep_num + 16));
    } else {
        proxy_endpointN_notify_mask |= (1 << ep_num);
    }
    endpointN_notify_mask = proxy_endpointN_notify_mask;

    LOG_DEBUGF(LOG_ENUM, "Endpoint %d configured successfully", ep_num);
}

// Configure all discovered endpoints
void USBDeviceProxy::configureAllEndpoints() {
    LOG_DEBUG(LOG_ENUM, "Configuring all endpoints...");
    
    for (uint8_t i = 0; i < num_endpoints; i++) {
        if (!endpoints[i].configured) {
            configureEndpoint(
                endpoints[i].address,
                endpoints[i].attributes,
                endpoints[i].maxPacketSize,
                endpoints[i].interval
            );
            endpoints[i].configured = true;
            endpoint_ready[i] = true; // Mark as ready to send data
        }
    }
    
    LOG_DEBUG(LOG_ENUM, "All endpoints configured");
}

// Handle CLEAR_FEATURE for endpoints
void USBDeviceProxy::handleClearFeature() {
    uint16_t feature = pending_setup.wValue;
    uint16_t endpoint = pending_setup.wIndex;
    
    LOG_DEBUGF(LOG_ENUM, "CLEAR_FEATURE - feature=%d endpoint=0x%04X", feature, endpoint);
    
    if (feature == 0) {  // ENDPOINT_HALT
        uint8_t ep_num = endpoint & 0x0F;
        uint8_t ep_dir = (endpoint & 0x80) ? 1 : 0;
        
        if (ep_num > 0 && ep_num <= 7) {
            // Clear any halt/stall condition
            volatile uint32_t* endptctrl = &USB1_ENDPTCTRL0 + ep_num;
            uint32_t ctrl = *endptctrl;
            
            if (ep_dir) {
                ctrl &= ~(1 << 16);  // Clear TXS (TX Stall)
            } else {
                ctrl &= ~(1 << 0);   // Clear RXS (RX Stall)
            }
            
            *endptctrl = ctrl;
            
            // Reset data toggle
            USB1_ENDPTFLUSH = ep_dir ? (1 << (ep_num + 16)) : (1 << ep_num);
            while (USB1_ENDPTFLUSH & (ep_dir ? (1 << (ep_num + 16)) : (1 << ep_num)));
            
            LOG_DEBUG(LOG_ENUM, "Endpoint halt cleared");
        }
    }
    
    // Send ZLP ACK
    receiveData(NULL, 0);
}

// Send data on a configured endpoint
void USBDeviceProxy::sendDataOnEndpoint(uint8_t ep_num, const uint8_t* data, uint32_t length) {
    if (ep_num == 0 || ep_num > 7) return;
    
    // Find the endpoint info
    int ep_idx = -1;
    for (int i = 0; i < num_endpoints; i++) {
        if ((endpoints[i].address & 0x0F) == ep_num && 
            (endpoints[i].address & 0x80)) {  // IN endpoint
            ep_idx = i;
            break;
        }
    }
    
    if (ep_idx < 0 || !endpoint_ready[ep_idx]) return;
    
    // Copy data to endpoint buffer
    if (length > sizeof(proxy_in_buffers[ep_idx])) {
        length = sizeof(proxy_in_buffers[ep_idx]);
    }
    memcpy(proxy_in_buffers[ep_idx], data, length);
    
    // Setup transfer
    proxy_in_transfers[ep_idx].next = 1;
    proxy_in_transfers[ep_idx].status = (length << 16) | (1<<7) | (1<<15); // Active + IOC
    uint32_t addr = (uint32_t)proxy_in_buffers[ep_idx];
    proxy_in_transfers[ep_idx].pointer0 = addr;
    proxy_in_transfers[ep_idx].pointer1 = addr + 4096;
    proxy_in_transfers[ep_idx].pointer2 = addr + 8192;
    proxy_in_transfers[ep_idx].pointer3 = addr + 12288;
    proxy_in_transfers[ep_idx].pointer4 = addr + 16384;
    
    // Queue transfer
    proxy_endpoint_queue_head[ep_num * 2 + 1].next = (uint32_t)&proxy_in_transfers[ep_idx];
    proxy_endpoint_queue_head[ep_num * 2 + 1].status = 0;
    
    // Prime endpoint
    USB1_ENDPTPRIME |= (1 << (ep_num + 16));
    
    // Mark as busy until complete
    endpoint_ready[ep_idx] = false;
}

// Check if endpoint is ready to send data
bool USBDeviceProxy::isEndpointReady(uint8_t ep_num) {
    for (int i = 0; i < num_endpoints; i++) {
        if ((endpoints[i].address & 0x0F) == ep_num && 
            (endpoints[i].address & 0x80)) {  // IN endpoint
            return endpoint_ready[i];
        }
    }
    return false;
}