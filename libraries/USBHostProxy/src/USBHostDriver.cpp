// USBHostDriver.cpp
#include "USBHostDriver.h"
#include "USBDeviceProxy.h"
#include "SunBoxEEPROM.h"
#include "SunBoxStartup.h"
#include "SunBoxLogger.h"

//=============================================================================
// Constructor/Destructor
//=============================================================================

USBHostDriver::USBHostDriver(USBHost& host) 
    : usbHost(&host), device(nullptr), device_ready(false), device_claimed(false),
      connect_time(0), config_descriptor_len(0), config_descriptor_valid(false),
      config_num_interfaces(0), config_value(1), config_attributes(0xA0), config_max_power(0x31),
      interface_count(0),
      control_pending(false), control_complete(false), control_length_received(0),
      control_success(false), control_last_token(0), in_pipe(nullptr), out_pipe(nullptr),
      in_endpoint_addr(0), out_endpoint_addr(0), in_endpoint_size(0),
      out_endpoint_size(0), in_endpoint_interval(1), out_endpoint_interval(1),
      last_rx_length(0), new_data_available(false),
      data_callback(nullptr), data_transfers_paused(false),
      pending_in_transfer(false),
      deviceProxy(nullptr) {
    
    // Initialize buffers
    memset(rx_buffer, 0, sizeof(rx_buffer));
    memset(last_rx_buffer, 0, sizeof(last_rx_buffer));
    memset(control_buffer, 0, sizeof(control_buffer));
    memset(config_descriptor, 0, sizeof(config_descriptor));
    
    // Clear all pipes and transfers
    memset(mypipes, 0, sizeof(mypipes));
    memset(mytransfers, 0, sizeof(mytransfers));
    
    // Clear interface info
    memset(interfaces, 0, sizeof(interfaces));
    
    // Contribute resources to USB Host - CRITICAL!
    contribute_Pipes(mypipes, sizeof(mypipes)/sizeof(Pipe_t));
    contribute_Transfers(mytransfers, sizeof(mytransfers)/sizeof(Transfer_t));
    
    // Register with USB Host BEFORE it starts - CRITICAL!
    driver_ready_for_device(this);
}

USBHostDriver::~USBHostDriver() {
    logger.error("USBHostDriver destructor called - THIS SHOULD NOT HAPPEN!");
    if (device_claimed) {
        disconnect();
    }
}

//=============================================================================
// Core USB Methods
//=============================================================================

bool USBHostDriver::begin() {
    LOG_STARTUP(LOG_BOOT, "SunBox USB Host Driver initializing...");
    LOG_STARTUP(LOG_BOOT, "SunBox USB Host Driver ready for device...");
    return true;
}

bool USBHostDriver::claim(Device_t *dev, int type, const uint8_t *descriptors, uint32_t len) {
    LOG_DEBUG(LOG_ENUM, "");
    LOG_DEBUGF(LOG_ENUM, "USBHostDriver::claim() called - type: %d, len: %lu", type, len);
    
    // Type 0 = Device level (store reference but don't claim)
    if (type == CLAIM_REPORT) {
        LOG_DEBUG(LOG_ENUM, "Device level claim (type 0) - storing device reference");
        device = dev;
        LOG_DEBUGF(LOG_ENUM, "Device VID: 0x%04X, PID: 0x%04X", dev->idVendor, dev->idProduct);
        
        // Extract some configuration info from the device structure
        // Note: Full configuration descriptor comes in type 1 claim
        if (dev->bMaxPower > 0) {
            config_max_power = dev->bMaxPower;
        }
        if (dev->bmAttributes > 0) {
            config_attributes = dev->bmAttributes;
        }
        
        return false;  // Let enumeration continue
    }
    
    // Type 1 = Interface level (actual claiming)
    if (type == CLAIM_INTERFACE) {
        if (device_claimed) {
            logger.warningf("SunBox USB Host Driver warning, already has claimed device, skipping this one (VID:0x%04X PID:0x%04X)",
                          dev ? dev->idVendor : 0, dev ? dev->idProduct : 0);
            return false;
        }
        
        if (!dev || dev != device) {
            logger.error("SunBox USB Host Driver Error invalid device pointer or mismatch.");
            return false;
        }
        
        LOG_DEBUG(LOG_ENUM, "Interface level claim (type 1) - claiming device!");
        LOG_DEBUG(LOG_ENUM, "SunBox Host Driver beginning claim process...");

        LOG_DEBUGF(LOG_ENUM, "Device VID: 0x%04X, PID: 0x%04X", dev->idVendor, dev->idProduct);

        LOG_DEBUGF(LOG_ENUM, "Descriptors length: %lu", len);
        
        // STORE THE RAW CONFIGURATION DESCRIPTOR DATA
        if (len > 0 && descriptors != nullptr && len <= MAX_CONFIG_DESCRIPTOR_SIZE) {
            memcpy(config_descriptor, descriptors, len);
            config_descriptor_len = len;
            config_descriptor_valid = true;
            LOG_DEBUGF(LOG_ENUM, "Stored %d bytes of configuration descriptor data", len);
        } else {
            logger.error("Error Descriptor data too large or invalid, this will cause proxying to fail & is not safe to use until fixed by the developer. please report this error.");
            config_descriptor_valid = false;
        }
        
        // Update config values from device structure if available
        if (device) {
            if (device->bMaxPower > 0) config_max_power = device->bMaxPower;
            if (device->bmAttributes > 0) config_attributes = device->bmAttributes;
        }
        
        connect_time = millis();
        
        // Parse descriptors to find HID interfaces and endpoints
        if (len > 0 && descriptors != nullptr) {
            parseDescriptors(descriptors, len);
        } else {
            logger.error("Error, no device descriptors found. this will cause proxying to fail and is not secure, please report this error.");
            // Set some default endpoints for testing
            in_endpoint_addr = 1;
            in_endpoint_size = 64;
            in_endpoint_interval = 1;
        }
        
        // Check for forced interface selection in EEPROM
        ClaimConfig config;
        
        if (sunboxEEPROM.loadClaimConfig(config) && 
            config.vid == dev->idVendor && 
            config.pid == dev->idProduct) {
            
            LOG_STARTUP(LOG_CONNECT, "Found forced interface configuration");
            LOG_STARTUPF(LOG_CONNECT, "Using interface %d endpoint 0x%02X for device VID:0x%04X PID:0x%04X",
                          config.interface_num, config.endpoint_addr | 0x80, config.vid, config.pid);
            
            // Find the specified interface
            bool found = false;
            for (uint8_t i = 0; i < interface_count; i++) {
                if (interfaces[i].interface_num == config.interface_num) {
                    in_endpoint_addr = config.endpoint_addr;
                    in_endpoint_size = config.endpoint_size;
                    in_endpoint_interval = interfaces[i].in_endpoint_interval;
                    found = true;
                    break;
                }
            }
            
            if (!found) {
                logger.error("SunBox USB Host Driver Error - Specified interface not found! Falling back to automatic selection");
            }
        }
        
        // Claim the endpoints
        claimEndpoints();
        
        LOG_STARTUP(LOG_CONNECT, "SunBox Host Driver found physical device");
        
        device_claimed = true;
        device_ready = true;
        
        // Start reading data
        startReading();
        
        LOG_STARTUP(LOG_CONNECT, "SunBox Host Driver claimed device");
        
        return true;  // Claim this interface
    }
    
    logger.errorf("SunBox USB Host Driver Error: Unknown claim type: %d - not claiming", type);
    return false;
}

void USBHostDriver::disconnect() {
    device_ready = false;
    device_claimed = false;
    device = nullptr;
    in_pipe = nullptr;
    out_pipe = nullptr;
    interface_count = 0;
    data_transfers_paused = false;
    pending_in_transfer = false;
    config_descriptor_valid = false;
    config_descriptor_len = 0;
    config_num_interfaces = 0;
    config_value = 1;
    config_attributes = 0xA0;
    config_max_power = 0x31;

    // Clear endpoint info
    in_endpoint_addr = 0;
    out_endpoint_addr = 0;
    in_endpoint_size = 0;
    out_endpoint_size = 0;
    in_endpoint_interval = 1;
    out_endpoint_interval = 1;

    // Invalidate descriptor cache on device disconnect
    if (deviceProxy) {
        deviceProxy->invalidateDescriptorCache();
    }

    LOG_WARNING(LOG_CONNECT, "Device Disconnect detected, may cause instability..");
}

//=============================================================================
// Descriptor Parsing
//=============================================================================

void USBHostDriver::parseDescriptors(const uint8_t* descriptors, uint32_t len) {
    const uint8_t* p = descriptors;
    const uint8_t* end = descriptors + len;
    
    LOG_DEBUG(LOG_ENUM, "Parsing descriptors...");
    
    // Clear endpoint info before parsing
    in_endpoint_addr = 0;
    out_endpoint_addr = 0;
    in_endpoint_size = 0;
    out_endpoint_size = 0;
    in_endpoint_interval = 1;
    out_endpoint_interval = 1;
    interface_count = 0;
    
    uint8_t current_interface_idx = 0xFF;
    
    while (p < end) {
        uint8_t desc_len = p[0];
        uint8_t desc_type = p[1];
        
        if (p + desc_len > end) break;
        
        // Look for interface descriptor
        if (desc_type == 0x04 && desc_len >= 9) { // Interface descriptor
            if (interface_count < MAX_INTERFACES) {
                current_interface_idx = interface_count;
                InterfaceInfo* iface = &interfaces[interface_count];
                
                iface->interface_num = p[2];
                iface->interface_class = p[5];
                iface->interface_subclass = p[6];
                iface->interface_protocol = p[7];
                iface->is_hid = (iface->interface_class == 0x03);
                iface->has_in_endpoint = false;
                
                LOG_DEBUGF(LOG_ENUM, "Found interface %d, class: 0x%02X%s",
                            iface->interface_num, iface->interface_class,
                            iface->is_hid ? (iface->interface_protocol == 1 ? " (HID Keyboard)" : 
                                           iface->interface_protocol == 2 ? " (HID Mouse)" : " (HID)") : "");
                
                interface_count++;
            }
        }
        
        // Look for HID class descriptor
        if (desc_type == 0x21 && desc_len >= 9 && current_interface_idx < interface_count) {
            uint16_t report_length = p[7] | (p[8] << 8);
            interfaces[current_interface_idx].hid_desc_length = report_length;
            
            LOG_DEBUGF(LOG_ENUM, "HID descriptor found! Report length: %d for interface %d",
                        report_length, interfaces[current_interface_idx].interface_num);
        }
        
        // Look for endpoint descriptors
        if (desc_type == 0x05 && desc_len >= 7) { // Endpoint descriptor
            uint8_t ep_addr = p[2];
            uint8_t ep_attr = p[3];
            uint16_t ep_size = p[4] | (p[5] << 8);
            uint8_t ep_interval = p[6];
            
            LOG_DEBUGF(LOG_ENUM, "Found endpoint: addr=0x%02X attr=0x%02X size=%d interval=%d",
                        ep_addr, ep_attr, ep_size, ep_interval);
            
            // Check if it's interrupt endpoint
            if ((ep_attr & 0x03) == 0x03) {
                if (ep_addr & 0x80) {
                    // IN endpoint
                    if (!in_endpoint_addr) {
                        in_endpoint_addr = ep_addr & 0x7F;
                        in_endpoint_size = ep_size;
                        in_endpoint_interval = ep_interval;
                        LOG_DEBUG(LOG_ENUM, "Using as primary IN endpoint");
                    }
                    
                    // Also store for current interface
                    if (current_interface_idx < interface_count && 
                        !interfaces[current_interface_idx].has_in_endpoint) {
                        interfaces[current_interface_idx].in_endpoint_addr = ep_addr & 0x7F;
                        interfaces[current_interface_idx].in_endpoint_size = ep_size;
                        interfaces[current_interface_idx].in_endpoint_interval = ep_interval;
                        interfaces[current_interface_idx].has_in_endpoint = true;
                    }
                } else {
                    // OUT endpoint
                    if (!out_endpoint_addr) {
                        out_endpoint_addr = ep_addr;
                        out_endpoint_size = ep_size;
                        out_endpoint_interval = ep_interval;
                        LOG_DEBUG(LOG_ENUM, "Using as OUT endpoint");
                    }
                }
            }
        }
        
        p += desc_len;
    }
    
    LOG_DEBUGF(LOG_ENUM, "Parsing complete - %d interfaces found, IN endpoint: %d, OUT endpoint: %d",
                interface_count, in_endpoint_addr, out_endpoint_addr);
    
    // Store the number of interfaces for configuration descriptor reconstruction
    config_num_interfaces = interface_count;
}

//=============================================================================
// Endpoint Management
//=============================================================================

void USBHostDriver::claimEndpoints() {
    // Claim IN endpoint
    if (in_endpoint_addr) {
        LOG_DEBUGF(LOG_ENUM, "Creating IN pipe for endpoint %d", in_endpoint_addr);
        
        // Create interrupt pipe (type 3) for IN endpoint
        in_pipe = new_Pipe(device, 3, in_endpoint_addr, 1, in_endpoint_size, in_endpoint_interval);
        if (in_pipe) {
            in_pipe->callback_function = in_callback;
            LOG_DEBUG(LOG_ENUM, "IN pipe created successfully");
        } else {
            logger.error("SunBox failed to create IN Pipe, this will cause proxying to fail and is not safe until fixed by the developer. report this error immediately.");
        }
    }
    
    // Claim OUT endpoint
    if (out_endpoint_addr) {
        LOG_DEBUGF(LOG_ENUM, "Creating OUT pipe for endpoint %d", out_endpoint_addr);
        
        // Create interrupt pipe (type 3) for OUT endpoint
        out_pipe = new_Pipe(device, 3, out_endpoint_addr, 0, out_endpoint_size, out_endpoint_interval);
        if (out_pipe) {
            LOG_DEBUG(LOG_ENUM, "OUT pipe created successfully");
        } else {
            logger.error("SunBox failed to create OUT Pipe, this will cause proxying to fail and is not safe until fixed by the developer. report this error immediately.");
        }
    }
}

void USBHostDriver::startReading() {
    if (in_pipe && !data_transfers_paused && !pending_in_transfer) {
        LOG_DEBUG(LOG_ENUM, "Starting data reading...");
        LOG_DEBUGF(LOG_ENUM, "IN pipe address: 0x%08X", (uint32_t)in_pipe);
        LOG_DEBUGF(LOG_ENUM, "Buffer address: 0x%08X", (uint32_t)rx_buffer);
        LOG_DEBUGF(LOG_ENUM, "Endpoint size: %d", in_endpoint_size);

        pending_in_transfer = true;
        queue_Data_Transfer(in_pipe, rx_buffer, in_endpoint_size, this);
        LOG_DEBUG(LOG_ENUM, "Data transfer queued");

        LOG_DEBUGF(LOG_ENUM, "Pipe callback function: 0x%08X", (uint32_t)in_pipe->callback_function);
    }
}

//=============================================================================
// Control Transfers
//=============================================================================

bool USBHostDriver::controlTransfer(uint8_t bmRequestType, uint8_t bRequest, 
                                   uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                                   uint8_t* data, uint16_t* actualLength, 
                                   uint32_t timeout_ms) {
    if (!device || !device_claimed) {
        logger.error("SunBox USB Host Driver Error: Control transfer failed - no device");
        return false;
    }
    
    // Always print debug for descriptor requests
    // logger.debugf("Control transfer: bmRequestType=0x%02X bRequest=0x%02X wValue=0x%04X wIndex=0x%04X wLength=%d",
    //             bmRequestType, bRequest, wValue, wIndex, wLength);
    
    // Log timestamp for timing analysis
    
    // // Log raw setup packet bytes - commented out for production
    // uint8_t setup_bytes[8];
    // setup_bytes[0] = bmRequestType;
    // setup_bytes[1] = bRequest;
    // setup_bytes[2] = wValue & 0xFF;
    // setup_bytes[3] = (wValue >> 8) & 0xFF;
    // setup_bytes[4] = wIndex & 0xFF;
    // setup_bytes[5] = (wIndex >> 8) & 0xFF;
    // setup_bytes[6] = wLength & 0xFF;
    // setup_bytes[7] = (wLength >> 8) & 0xFF;
    
    // String setupStr = "Setup packet bytes: ";
    // for (int i = 0; i < 8; i++) {
    //     if (i > 0) setupStr += " ";
    //     if (setup_bytes[i] < 0x10) setupStr += "0";
    //     setupStr += String(setup_bytes[i], HEX);
    // }
    // logger.debug(setupStr.c_str());
    
    // Control transfer delay removed - was adding ~40-70ms to enumeration
    // Original: delay(5) per control transfer, fired unconditionally
    // pauseDataTransfers() already handles draining in-flight transfers when needed
    
    // Setup the control transfer
    control_setup.bmRequestType = bmRequestType;
    control_setup.bRequest = bRequest;
    control_setup.wValue = wValue;
    control_setup.wIndex = wIndex;
    control_setup.wLength = min(wLength, (uint16_t)512);
    
    // Reset state
    control_pending = true;
    control_complete = false;
    control_success = false;
    control_length_received = 0;
    control_last_token = 0;
    
    // CRITICAL FIX: For OUT transfers, ensure data is in control_buffer
    if (!(bmRequestType & 0x80) && wLength > 0 && data != nullptr) {
        LOG_DEBUGF(LOG_ENUM, "OUT transfer - copying %d bytes to control buffer", wLength);
        
        // Copy the provided data to control_buffer
        memcpy(control_buffer, data, min(wLength, sizeof(control_buffer)));
        
        // Debug: Print first few bytes of OUT data
        if (logger.isChannelEnabled(LOG_ENUM)) {
            String outDataStr = "OUT data: ";
            for (int i = 0; i < min(wLength, (uint16_t)16); i++) {
                if (control_buffer[i] < 0x10) outDataStr += "0";
                outDataStr += String(control_buffer[i], HEX) + " ";
            }
            if (wLength > 16) outDataStr += "...";
            logger.debug(outDataStr.c_str());
        }
    }
    
    // Queue the transfer - always use control_buffer
    // logger.debug("Queuing control transfer");
    // logger.debugf("Device address: %d, control pipe: %p", device ? device->address : -1, device ? device->control_pipe : nullptr);
    bool queue_result = queue_Control_Transfer(device, &control_setup, control_buffer, this);
    // logger.debug(queue_result ? "...success" : "...failed");
    
    if (!queue_result) {
        logger.error("Failed to queue control transfer!");
        return false;
    }
    
    // Wait for completion
    uint32_t wait_start = millis();
    while (!control_complete && (millis() - wait_start) < timeout_ms) {
        // Process USB tasks while waiting
        if (usbHost) {
            usbHost->Task();
        }
        yield();
    }
    
    
    // Check completion status
    if (!control_complete) {
        logger.warningf("Control transfer timeout after %lums", timeout_ms);
        control_pending = false;  // Reset state
        return false;
    }
    
    if (!control_success) {
        logger.errorf("SunBox USB Host Driver Error: Control transfer failed, token=0x%08X", control_last_token);
        return false;
    }
    
    // For OUT transfers, we might get 0 bytes back (just ACK)
    if (!(bmRequestType & 0x80)) {
        LOG_DEBUG(LOG_ENUM, "OUT transfer completed successfully");
        if (actualLength) *actualLength = 0;
        return true;
    }
    
    if (control_length_received == 0 && wLength > 0) {
        logger.warning("Control transfer returned no data");
        return false;
    }
    
    // Success - copy data for IN transfers
    LOG_DEBUGF(LOG_ENUM, "Control transfer complete, received %d bytes", control_length_received);
    
    if (data && control_length_received > 0) {
        uint16_t copy_len = min(control_length_received, wLength);
        memcpy(data, control_buffer, copy_len);
        if (actualLength) *actualLength = copy_len;
    } else if (actualLength) {
        *actualLength = 0;
    }
    
    return true;
}

void USBHostDriver::control(const Transfer_t *transfer) {
    // delayMicroseconds(100) removed - was adding ~0.1ms per control transfer callback
    
    // logger.debug("control() callback called");
    
    if (control_pending && transfer->buffer == control_buffer) {
        control_pending = false;
        control_complete = true;
        control_length_received = transfer->length;
        
        // Check for errors in the transfer
        uint32_t token = transfer->qtd.token;
        control_last_token = token;
        uint8_t status = (token >> 0) & 0xFF;
        
        // SOLUTION 1: Accept transfers despite ping error
        // This handles the Razer Viper V3 Pro wireless false positive
        // Handle both IN transfers with data and OUT transfers
        if (status == 0x01) {
            // Only Ping bit set - check transfer type
            uint8_t pid_code = (token >> 8) & 0x03;
            
            // IN transfer with data received
            if (control_length_received > 0) {
                control_success = true;
                LOG_DEBUG(LOG_ENUM, "WORKAROUND: IN transfer - Treating Ping-only error as success since data was received");
                LOG_DEBUGF(LOG_ENUM, "Token=0x%08X, Status=0x01 (Ping only), Length=%d bytes",
                             token, control_length_received);
            }
            // OUT transfer (PID=1) with ping error  
            else if (pid_code == 1) {
                control_success = true;
                LOG_DEBUG(LOG_ENUM, "WORKAROUND: OUT transfer - Treating Ping-only error as success for SET_CONFIGURATION");
                LOG_DEBUGF(LOG_ENUM, "Token=0x%08X, Status=0x01 (Ping only), PID=%d (OUT)", token, pid_code);
            }
            else {
                // Some other case with ping bit set
                control_success = false;
                LOG_DEBUGF(LOG_ENUM, "Ping error without data or OUT transfer, token=0x%08X", token);
            }
        } else {
            // Normal success check - status byte should be 0
            control_success = (status == 0);
        }
        
        // logger.debugf("Control transfer completed with token=0x%08X, length=%d", token, control_length_received);
        
        if (!control_success) {
            logger.errorf("SunBox USB Host Driver Error: Control transfer error, token=0x%08X", token);
            
            // Commented out verbose token decoding for production
            // uint8_t status = (token >> 0) & 0xFF;
            // uint8_t pid_code = (token >> 8) & 0x03;
            // uint8_t cerr = (token >> 10) & 0x03;  // Error counter
            // uint8_t page = (token >> 12) & 0x07;  // Page select
            // uint8_t ioc = (token >> 15) & 0x01;   // Interrupt on complete
            // uint16_t total_bytes = (token >> 16) & 0x7FFF;  // Total bytes to transfer
            // uint8_t dt = (token >> 31) & 0x01;    // Data toggle
            
            // logger.debugf("Token decode: Status=0x%02X, PID=%d, CERR=%d, Page=%d, IOC=%d, TotalBytes=%d, DT=%d",
            //             status, pid_code, cerr, page, ioc, total_bytes, dt);
            
            // // Decode status bits
            // uint8_t active = (status >> 7) & 0x01;
            // uint8_t halted = (status >> 6) & 0x01;
            // uint8_t data_buffer_err = (status >> 5) & 0x01;
            // uint8_t babble = (status >> 4) & 0x01;
            // uint8_t xact_err = (status >> 3) & 0x01;
            // uint8_t missed_micro = (status >> 2) & 0x01;
            // uint8_t split_state = (status >> 1) & 0x01;
            // uint8_t ping_state = status & 0x01;
            
            // logger.debugf("Status bits: Active=%d, Halted=%d, DataBufErr=%d, Babble=%d, XactErr=%d, MissedMicro=%d, Split=%d, Ping=%d",
            //             active, halted, data_buffer_err, babble, xact_err, missed_micro, split_state, ping_state);
            
            // // Log control buffer contents for failed transfers
            // if (control_length_received > 0) {
            //     String bufStr = "Control buffer (first 16 bytes): ";
            //     for (uint16_t i = 0; i < 16 && i < control_length_received; i++) {
            //         if (i > 0) bufStr += " ";
            //         if (control_buffer[i] < 0x10) bufStr += "0";
            //         bufStr += String(control_buffer[i], HEX);
            //     }
            //     logger.debug(bufStr.c_str());
                
            //     // Additional check - if we have data but status shows only ping error, warn about it
            //     if (status == 0x01) {
            //         logger.warning("NOTE: This looks like a false ping error - data is valid but not being used!");
            //     }
            // }
        }
    } else {
        LOG_DEBUG(LOG_ENUM, "control() called but not for our transfer");
    }
}

//=============================================================================
// Transfer Control Methods
//=============================================================================

void USBHostDriver::pauseDataTransfers() { 
    if (data_transfers_paused) {
        LOG_DEBUG(LOG_DATA, "Already paused");
        return;
    }
    
    data_transfers_paused = true;
    LOG_DEBUG(LOG_DATA, "Data transfers paused");
    
    // Wait for any pending transfer to complete without queuing a new one
    if (pending_in_transfer) {
        LOG_DEBUG(LOG_DATA, "Waiting for current transfer to complete...");
        uint32_t wait_start = millis();
        
        // Wait up to 20ms for the transfer to complete naturally
        while (pending_in_transfer && (millis() - wait_start) < 20) {
            if (usbHost) {
                usbHost->Task();
            }
            yield();
            delayMicroseconds(100);  // Check more frequently
        }
        
        if (!pending_in_transfer) {
            LOG_DEBUG(LOG_DATA, "Current transfer completed");
        } else {
            LOG_DEBUG(LOG_DATA, "Transfer still pending after wait");
            // Force clear the flag since we've waited long enough
            pending_in_transfer = false;
        }
    }
    
    // Stabilization delay removed - busy-wait above already ensures pending transfers complete
    // Original: delay(5) after pauseDataTransfers() wait loop
    LOG_DEBUG(LOG_DATA, "Pause complete");
}

void USBHostDriver::resumeDataTransfers() { 
    data_transfers_paused = false; 
    LOG_DEBUG(LOG_DATA, "Data transfers resumed");
    
    // Immediately queue a new transfer if we have a pipe and device is ready
    if (in_pipe && device_ready && !pending_in_transfer) {
        pending_in_transfer = true;
        queue_Data_Transfer(in_pipe, rx_buffer, in_endpoint_size, this);
        LOG_DEBUG(LOG_DATA, "Queued new transfer after resume");
    }
}

//=============================================================================
// Data Transfer Methods
//=============================================================================

void USBHostDriver::in_callback(const Transfer_t *transfer) {
    if (!transfer) return;
    
    USBHostDriver* driver = (USBHostDriver*)transfer->driver;
    if (driver) {
        driver->processInData(transfer);
    }
}

void USBHostDriver::processInData(const Transfer_t *transfer) {
    // Clear pending flag first
    pending_in_transfer = false;
    
    if (!transfer || !transfer->buffer) {
        logger.warning("Invalid transfer or buffer!");
        return;
    }
    
    // Calculate actual received length
    uint32_t len = transfer->length;
    uint32_t token = transfer->qtd.token;
    uint32_t bytes_not_transferred = (token >> 16) & 0x7FFF;
    uint32_t actual_len = len - bytes_not_transferred;
    
    if (actual_len > len) {
        actual_len = len;  // Sanity check
    }
    
    if (actual_len > 0 && actual_len <= RX_BUFFER_SIZE) {
        // Debug first data packet
        static bool first_data = true;
        if (first_data) {
            if (logger.isChannelEnabled(LOG_CONNECT)) {
                // Build hex string for data
                String dataStr = "";
                for (uint32_t i = 0; i < actual_len && i < 8; i++) {
                    if (i > 0) dataStr += " ";
                    if (((uint8_t*)transfer->buffer)[i] < 0x10) dataStr += "0";
                    dataStr += String(((uint8_t*)transfer->buffer)[i], HEX);
                }
                logger.startupf("Received first mouse data packet, length: %lu, data: %s, device should now be fully operational.",
                              actual_len, dataStr.c_str());
            }
            first_data = false;
        }
        
        // Copy to last data buffer
        memcpy(last_rx_buffer, transfer->buffer, actual_len);
        last_rx_length = actual_len;
        new_data_available = true;
        
        // Call callback if registered
        if (data_callback) {
            data_callback(last_rx_buffer, actual_len);
        }
    }
    
    // DEBUG: Log transfer state when mouse data is received
    static uint32_t data_packet_count = 0;
    data_packet_count++;
    
    // Only log every 100 packets to reduce spam
    if ((data_packet_count % 100) == 1) {
        LOG_DEBUGF(LOG_DATA, "Mouse packet #%lu len=%lu available=%s proxy_configured=true",
                    data_packet_count, actual_len, new_data_available ? "true" : "false");
    }
    
    // Always log the detailed state for the first few packets
    if (data_packet_count <= 5) {
        LOG_DEBUGF(LOG_DATA, "processInData #%lu paused=%s ready=%s pipe=%s",
                    data_packet_count,
                    data_transfers_paused ? "true" : "false",
                    device_ready ? "true" : "false",
                    in_pipe ? "valid" : "null");
    }
    
    // IMPORTANT: Queue next transfer ONLY if not paused
    if (in_pipe && !data_transfers_paused && device_ready) {
        pending_in_transfer = true;
        queue_Data_Transfer(in_pipe, rx_buffer, in_endpoint_size, this);
        
        if (data_packet_count <= 5) {
            LOG_DEBUG(LOG_DATA, "Queued next transfer");
        }
    } else if (data_transfers_paused) {
        logger.warning("Not queuing - transfers paused!");
    } else {
        logger.warningf("Not queuing - pipe=%s ready=%s",
                      in_pipe ? "valid" : "null",
                      device_ready ? "true" : "false");
    }
}

bool USBHostDriver::sendData(const uint8_t* data, uint32_t length) {
    if (!out_pipe || !data || length == 0) {
        return false;
    }
    
    // For now, just return false - implement when needed
    return false;
}

bool USBHostDriver::getLastData(uint8_t* buffer, uint32_t& length) {
    if (!new_data_available) {
        return false;
    }
    
    memcpy(buffer, last_rx_buffer, last_rx_length);
    length = last_rx_length;
    new_data_available = false;
    
    return true;
}

//=============================================================================
// Interface Information Methods
//=============================================================================

uint8_t USBHostDriver::getInterfaceNumber(uint8_t index) const {
    if (index < interface_count) {
        return interfaces[index].interface_num;
    }
    return 0xFF;
}

uint8_t USBHostDriver::getInterfaceClass(uint8_t index) const {
    if (index < interface_count) {
        return interfaces[index].interface_class;
    }
    return 0;
}

uint8_t USBHostDriver::getInterfaceSubclass(uint8_t index) const {
    if (index < interface_count) {
        return interfaces[index].interface_subclass;
    }
    return 0;
}

uint8_t USBHostDriver::getInterfaceProtocol(uint8_t index) const {
    if (index < interface_count) {
        return interfaces[index].interface_protocol;
    }
    return 0;
}

uint16_t USBHostDriver::getHIDDescriptorLength(uint8_t interface_index) const {
    if (interface_index < interface_count) {
        return interfaces[interface_index].hid_desc_length;
    }
    return 0;
}

bool USBHostDriver::isHIDInterface(uint8_t interface_index) const {
    if (interface_index < interface_count) {
        return interfaces[interface_index].is_hid;
    }
    return false;
}

int8_t USBHostDriver::findInterface(uint8_t class_code, uint8_t subclass, uint8_t protocol) const {
    for (uint8_t i = 0; i < interface_count; i++) {
        if (interfaces[i].interface_class == class_code) {
            if (subclass != 0xFF && interfaces[i].interface_subclass != subclass) continue;
            if (protocol != 0xFF && interfaces[i].interface_protocol != protocol) continue;
            return i;
        }
    }
    return -1;
}

uint8_t USBHostDriver::getEndpointAddress(uint8_t interface_index) const {
    if (interface_index < interface_count && interfaces[interface_index].has_in_endpoint) {
        return interfaces[interface_index].in_endpoint_addr;
    }
    return 0;
}

uint16_t USBHostDriver::getEndpointSize(uint8_t interface_index) const {
    if (interface_index < interface_count && interfaces[interface_index].has_in_endpoint) {
        return interfaces[interface_index].in_endpoint_size;
    }
    return 0;
}

uint8_t USBHostDriver::getEndpointInterval(uint8_t interface_index) const {
    if (interface_index < interface_count && interfaces[interface_index].has_in_endpoint) {
        return interfaces[interface_index].in_endpoint_interval;
    }
    return 0;
}

//=============================================================================
// Device Speed Detection - NEW METHOD
//=============================================================================

// Get the actual EP0 max packet size from the device descriptor
uint8_t USBHostDriver::getDeviceEP0Size() const {
    if (!device || !device_ready) {
        logger.warning("No device connected, defaulting EP0 size to 64 bytes");
        return 64; // Default to 64 if no device
    }
    
    // We need to get this from the device descriptor
    // The bMaxPacketSize0 is at offset 7 in the device descriptor
    uint8_t descriptor[18];
    uint16_t actual_len = 0;
    
    // Request device descriptor from the device
    bool success = const_cast<USBHostDriver*>(this)->controlTransfer(
        0x80,  // bmRequestType: Device-to-host, standard, device
        0x06,  // bRequest: GET_DESCRIPTOR
        0x0100,  // wValue: Device descriptor (type 1, index 0)
        0,     // wIndex
        18,    // wLength: Standard device descriptor length
        descriptor,
        &actual_len,
        100    // timeout
    );
    
    if (!success || actual_len < 8) {
        logger.error("SunBox USB Host Driver Error: Failed to get device descriptor for EP0 size - defaulting to 64");
        return 64;
    }
    
    // Extract bMaxPacketSize0 from offset 7
    uint8_t size = descriptor[7];
    
    // Sanity check - EP0 can only be 8, 16, 32, or 64 bytes per USB spec
    if (size != 8 && size != 16 && size != 32 && size != 64) {
        logger.errorf("SunBox USB Host Driver Error: Invalid EP0 size %d bytes from device - defaulting to 64", size);
        return 64;
    }
    
    LOG_DEBUGF(LOG_ENUM, "Device EP0 max packet size: %d bytes", size);
    
    return size;
}

// Get the actual device speed (Low/Full/High)
uint8_t USBHostDriver::getDeviceSpeed() const {
    if (!device || !device_ready) {
        logger.warning("No device connected, defaulting to High Speed");
        return 2; // Default to high speed if no device
    }
    
    // CRITICAL: Convert USBHost_t36 encoding to standard encoding
    // Library: 0=Full, 1=Low, 2=High
    // We need: 0=Low, 1=Full, 2=High
    
    uint8_t lib_speed = device->speed;
    uint8_t standard_speed;
    
    // Only log speed once using a static flag
    static bool speed_logged = false;
    static uint16_t last_vid_pid = 0;
    uint16_t current_vid_pid = (device->idVendor << 16) | device->idProduct;
    
    // Reset flag if device changed
    if (current_vid_pid != last_vid_pid) {
        speed_logged = false;
        last_vid_pid = current_vid_pid;
    }
    
    switch (lib_speed) {
        case 0:  // Library says Full Speed (12 Mbps)
            standard_speed = 1;
            if (!speed_logged) {
                LOG_STARTUP(LOG_CONNECT, "Found device with Speed Full Speed (12 Mbps)");
                speed_logged = true;
            }
            break;
        case 1:  // Library says Low Speed (1.5 Mbps)
            standard_speed = 0;
            if (!speed_logged) {
                LOG_STARTUP(LOG_CONNECT, "Found device with Speed Low Speed (1.5 Mbps)");
                speed_logged = true;
            }
            break;
        case 2:  // Library says High Speed (480 Mbps)
            standard_speed = 2;
            if (!speed_logged) {
                LOG_STARTUP(LOG_CONNECT, "Found device with Speed High Speed (480 Mbps)");
                speed_logged = true;
            }
            break;
        default:
            logger.errorf("Unknown speed encoding: %d", lib_speed);
            standard_speed = 1; // Default to Full Speed
            break;
    }
    
    return standard_speed;
}

// Keep the old method for compatibility but have it use the new one
bool USBHostDriver::isDeviceHighSpeed() const {
    return getDeviceSpeed() == 2;
}

//=============================================================================
// Debug Methods
//=============================================================================

void USBHostDriver::dumpDeviceInfo() {
    LOG_INFO(LOG_COMMAND, "\n=== USB Device Descriptor Dump ===");

    if (!device) {
        LOG_INFO(LOG_COMMAND, "No device connected!");
        return;
    }

    // Device info
    LOG_INFOF(LOG_COMMAND, "Device VID: 0x%04X PID: 0x%04X", device->idVendor, device->idProduct);
    LOG_INFOF(LOG_COMMAND, "Device Class: 0x%02X", device->bDeviceClass);

    // Interface summary
    LOG_INFOF(LOG_COMMAND, "\nTotal Interfaces: %d", interface_count);

    // If we have stored configuration descriptor, parse it for display
    if (config_descriptor_valid && config_descriptor_len > 0) {
        LOG_INFO(LOG_COMMAND, "\n[Using stored configuration descriptor]");
        displayStoredDescriptors();
    } else {
        // Fall back to basic interface info from our structures
        LOG_INFO(LOG_COMMAND, "\n[No stored descriptors available - showing parsed data only]");

        // Dump each interface
        for (uint8_t i = 0; i < interface_count; i++) {
            LOG_INFOF(LOG_COMMAND, "\n--- Interface %d ---", interfaces[i].interface_num);

            LOG_INFOF(LOG_COMMAND, "  Class: 0x%02X%s", interfaces[i].interface_class,
                        interfaces[i].is_hid ?
                        (interfaces[i].interface_protocol == 0 ? " (HID - None)" :
                         interfaces[i].interface_protocol == 1 ? " (HID - Keyboard)" :
                         interfaces[i].interface_protocol == 2 ? " (HID - Mouse)" : " (HID - Other)") : "");

            LOG_INFOF(LOG_COMMAND, "  Subclass: 0x%02X", interfaces[i].interface_subclass);
            LOG_INFOF(LOG_COMMAND, "  Protocol: 0x%02X", interfaces[i].interface_protocol);

            if (interfaces[i].is_hid && interfaces[i].hid_desc_length > 0) {
                LOG_INFOF(LOG_COMMAND, "  HID Descriptor Length: %d bytes", interfaces[i].hid_desc_length);
            }

            if (interfaces[i].has_in_endpoint) {
                LOG_INFOF(LOG_COMMAND, "  IN Endpoint: 0x%02X (%d bytes, interval %dms)",
                            interfaces[i].in_endpoint_addr | 0x80,
                            interfaces[i].in_endpoint_size,
                            interfaces[i].in_endpoint_interval);
            } else {
                LOG_INFO(LOG_COMMAND, "  No IN endpoint");
            }
        }
    }

    // Current configuration
    LOG_INFO(LOG_COMMAND, "\n--- Current Configuration ---");
    if (in_endpoint_addr) {
        LOG_INFOF(LOG_COMMAND, "Primary IN Endpoint: 0x%02X (%d bytes)",
                    in_endpoint_addr | 0x80, in_endpoint_size);
    } else {
        LOG_INFO(LOG_COMMAND, "Primary IN Endpoint: None");
    }

    if (out_endpoint_addr) {
        LOG_INFOF(LOG_COMMAND, "Primary OUT Endpoint: 0x%02X (%d bytes)",
                    out_endpoint_addr, out_endpoint_size);
    } else {
        LOG_INFO(LOG_COMMAND, "Primary OUT Endpoint: None");
    }

    LOG_INFO(LOG_COMMAND, "\n=================================");
}

void USBHostDriver::displayStoredDescriptors() {
    // First, display the reconstructed Configuration Descriptor header
    LOG_INFO(LOG_COMMAND, "\n--- Configuration Descriptor (Reconstructed) ---");
    LOG_INFOF(LOG_COMMAND, "  Total Length: %d bytes", config_descriptor_len + 9);
    LOG_INFOF(LOG_COMMAND, "  Number of Interfaces: %d", config_num_interfaces);
    LOG_INFOF(LOG_COMMAND, "  Configuration Value: %d", config_value);
    LOG_INFO(LOG_COMMAND, "  Configuration String: 0");
    LOG_INFOF(LOG_COMMAND, "  Attributes: 0x%02X (%s%s%s)",
                config_attributes,
                (config_attributes & 0x80) ? "Bus Powered " : "",
                (config_attributes & 0x40) ? "Self Powered " : "",
                (config_attributes & 0x20) ? "Remote Wakeup " : "");
    LOG_INFOF(LOG_COMMAND, "  Max Power: %d mA", config_max_power * 2);

    // Now parse the stored interface and endpoint descriptors
    const uint8_t* p = config_descriptor;
    const uint8_t* end = config_descriptor + config_descriptor_len;
    uint8_t current_interface = 0xFF;

    while (p < end) {
        uint8_t desc_len = p[0];
        uint8_t desc_type = p[1];

        if (p + desc_len > end) break;

        // Interface descriptor
        if (desc_type == 0x04 && desc_len >= 9) {
            current_interface = p[2];
            LOG_INFOF(LOG_COMMAND, "\n--- Interface %d ---", current_interface);
            LOG_INFOF(LOG_COMMAND, "  Alternate Setting: %d", p[3]);
            LOG_INFOF(LOG_COMMAND, "  Number of Endpoints: %d", p[4]);
            LOG_INFOF(LOG_COMMAND, "  Class: 0x%02X%s", p[5],
                        p[5] == 0x03 ?
                        (p[7] == 0 ? " (HID - None)" :
                         p[7] == 1 ? " (HID - Keyboard)" :
                         p[7] == 2 ? " (HID - Mouse)" : " (HID - Other)") : "");
            LOG_INFOF(LOG_COMMAND, "  Subclass: 0x%02X", p[6]);
            LOG_INFOF(LOG_COMMAND, "  Protocol: 0x%02X", p[7]);
            LOG_INFOF(LOG_COMMAND, "  Interface String: %d", p[8]);
        }

        // HID class descriptor
        else if (desc_type == 0x21 && desc_len >= 9) {
            LOG_INFO(LOG_COMMAND, "  HID Class Descriptor:");
            LOG_INFOF(LOG_COMMAND, "    HID Version: %d.%d", p[3], p[2]);
            LOG_INFOF(LOG_COMMAND, "    Country Code: %d", p[4]);
            LOG_INFOF(LOG_COMMAND, "    Number of Descriptors: %d", p[5]);

            // Parse descriptor info
            for (uint8_t i = 0; i < p[5] && (6 + i*3 + 2) < desc_len; i++) {
                uint8_t dtype = p[6 + i*3];
                uint16_t dlen = p[7 + i*3] | (p[8 + i*3] << 8);
                LOG_INFOF(LOG_COMMAND, "    Descriptor %d: Type=0x%02X%s, Length=%d bytes",
                            i, dtype,
                            dtype == 0x22 ? " (Report)" : dtype == 0x23 ? " (Physical)" : "",
                            dlen);
            }
        }

        // Endpoint descriptor
        else if (desc_type == 0x05 && desc_len >= 7) {
            LOG_INFOF(LOG_COMMAND, "  Endpoint 0x%02X (%s):",
                        p[2], (p[2] & 0x80) ? "IN" : "OUT");

            const char* ep_type = "";
            switch (p[3] & 0x03) {
                case 0: ep_type = "Control"; break;
                case 1: ep_type = "Isochronous"; break;
                case 2: ep_type = "Bulk"; break;
                case 3: ep_type = "Interrupt"; break;
            }
            LOG_INFOF(LOG_COMMAND, "    Attributes: 0x%02X (%s)", p[3], ep_type);
            LOG_INFOF(LOG_COMMAND, "    Max Packet Size: %d bytes", p[4] | (p[5] << 8));
            LOG_INFOF(LOG_COMMAND, "    Interval: %d ms", p[6]);
        }

        // Unknown descriptor
        else {
            LOG_INFOF(LOG_COMMAND, "\n  Unknown Descriptor Type 0x%02X, Length=%d", desc_type, desc_len);
        }

        p += desc_len;
    }

    // Display raw hex dump at the end
    LOG_INFO(LOG_COMMAND, "\n--- Raw Configuration Descriptor Hex Dump ---");
    LOG_INFO(LOG_COMMAND, "  [Reconstructed header + stored descriptors]");

    if (logger.isChannelEnabled(LOG_COMMAND)) {
        // First show the reconstructed configuration descriptor header
        String headerStr = "  000: 09 02 ";
        uint16_t total_len = config_descriptor_len + 9;
        if ((total_len & 0xFF) < 0x10) headerStr += "0";
        headerStr += String(total_len & 0xFF, HEX) + " ";
        if (((total_len >> 8) & 0xFF) < 0x10) headerStr += "0";
        headerStr += String((total_len >> 8) & 0xFF, HEX) + " ";
        if (config_num_interfaces < 0x10) headerStr += "0";
        headerStr += String(config_num_interfaces, HEX) + " ";
        if (config_value < 0x10) headerStr += "0";
        headerStr += String(config_value, HEX) + " 00 ";  // Config string index
        headerStr += String(config_attributes, HEX) + " ";
        headerStr += String(config_max_power, HEX);
        headerStr += "                  [Config Header]";
        logger.info(headerStr.c_str());

        // Then show the stored descriptors
        for (uint16_t i = 0; i < config_descriptor_len; i += 16) {
            String lineStr = "  ";
            uint16_t addr = i + 9;  // Account for header
            if (addr < 0x10) lineStr += "0";
            if (addr < 0x100) lineStr += "0";
            lineStr += String(addr, HEX) + ": ";

            // Hex bytes
            for (uint16_t j = 0; j < 16 && (i + j) < config_descriptor_len; j++) {
                if (config_descriptor[i + j] < 0x10) lineStr += "0";
                lineStr += String(config_descriptor[i + j], HEX) + " ";
            }

            // Padding if last line
            if (config_descriptor_len - i < 16) {
                for (uint16_t j = config_descriptor_len - i; j < 16; j++) {
                    lineStr += "   ";
                }
            }

            lineStr += " ";

            // ASCII representation
            for (uint16_t j = 0; j < 16 && (i + j) < config_descriptor_len; j++) {
                char c = config_descriptor[i + j];
                if (c >= ' ' && c <= '~') {
                    lineStr += c;
                } else {
                    lineStr += ".";
                }
            }

            logger.info(lineStr.c_str());
        }

        logger.infof("Total bytes: %d (9 header + %d descriptors)", total_len, config_descriptor_len);
    }
}