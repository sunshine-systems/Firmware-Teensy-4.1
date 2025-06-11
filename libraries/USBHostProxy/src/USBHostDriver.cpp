// USBHostDriver.cpp
#include "USBHostDriver.h"
#include "SunBoxEEPROM.h"
#include "SunBoxStartup.h"

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
      pending_in_transfer(false) {  // Added new member
    
    Serial4.println("S: USBHostDriver constructor called");
    
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
    Serial4.println("S: Contributing Pipes and Transfers to USB Host");
    contribute_Pipes(mypipes, sizeof(mypipes)/sizeof(Pipe_t));
    contribute_Transfers(mytransfers, sizeof(mytransfers)/sizeof(Transfer_t));
    
    // Register with USB Host BEFORE it starts - CRITICAL!
    Serial4.println("S: Registering driver with USB Host (in constructor)");
    driver_ready_for_device(this);
    
    Serial4.println("S: USBHostDriver constructor complete");
}

USBHostDriver::~USBHostDriver() {
    Serial4.println("S: USBHostDriver destructor called - THIS SHOULD NOT HAPPEN!");
    if (device_claimed) {
        disconnect();
    }
}

//=============================================================================
// Core USB Methods
//=============================================================================

bool USBHostDriver::begin() {
    Serial4.println("S: SunBox Host Driver begin() called.");
    Serial4.println("S: SunBox Host Driver ready to receive devices.");
    return true;
}

bool USBHostDriver::claim(Device_t *dev, int type, const uint8_t *descriptors, uint32_t len) {
    Serial4.println();
    Serial4.print("S: USBHostDriver::claim() called - type: ");
    Serial4.print(type);
    Serial4.print(", len: ");
    Serial4.println(len);
    
    // Type 0 = Device level (store reference but don't claim)
    if (type == CLAIM_REPORT) {
        Serial4.println("S: Device level claim (type 0) - storing device reference");
        device = dev;
        Serial4.print("S: Device VID: 0x");
        Serial4.print(dev->idVendor, HEX);
        Serial4.print(", PID: 0x");
        Serial4.println(dev->idProduct, HEX);
        
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
            Serial4.println("S: Already have a claimed device, skipping");
            return false;
        }
        
        if (!dev || dev != device) {
            Serial4.println("S: Invalid device pointer or mismatch");
            return false;
        }
        
        Serial4.println("S: Interface level claim (type 1) - claiming device!");
        Serial4.println("S: SunBox Host Driver beginning claim process...");
        
        Serial4.print("S: Device VID: 0x");
        Serial4.print(dev->idVendor, HEX);
        Serial4.print(", PID: 0x");
        Serial4.println(dev->idProduct, HEX);
        
        Serial4.print("S: Descriptors length: ");
        Serial4.println(len);
        
        // STORE THE RAW CONFIGURATION DESCRIPTOR DATA
        if (len > 0 && descriptors != nullptr && len <= MAX_CONFIG_DESCRIPTOR_SIZE) {
            memcpy(config_descriptor, descriptors, len);
            config_descriptor_len = len;
            config_descriptor_valid = true;
            Serial4.print("S: Stored ");
            Serial4.print(len);
            Serial4.println(" bytes of configuration descriptor data");
        } else {
            Serial4.println("S: WARNING: Descriptor data too large or invalid!");
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
            Serial4.println("S: No descriptors provided, using defaults");
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
            
            Serial4.println("S: Found forced interface configuration!");
            Serial4.print("S: Using interface ");
            Serial4.print(config.interface_num);
            Serial4.print(" endpoint 0x");
            Serial4.print(config.endpoint_addr | 0x80, HEX);
            Serial4.print(" for device VID:0x");
            Serial4.print(config.vid, HEX);
            Serial4.print(" PID:0x");
            Serial4.println(config.pid, HEX);
            
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
                Serial4.println("S: ERROR - Specified interface not found!");
                Serial4.println("S: Falling back to automatic selection");
            }
        }
        
        // Claim the endpoints
        claimEndpoints();
        
        Serial4.println("S: SunBox Host Driver found device");
        
        device_claimed = true;
        device_ready = true;
        
        // Start reading data
        startReading();
        
        Serial4.println("S: SunBox Host Driver claim process complete.");
        Serial4.println("S: Device successfully claimed!");
        
        return true;  // Claim this interface
    }
    
    Serial4.print("S: Unknown claim type: ");
    Serial4.print(type);
    Serial4.print(" - not claiming");
    Serial4.println();
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
    
    Serial4.println("S: SunBox Host Driver device disconnected.");
}

//=============================================================================
// Descriptor Parsing
//=============================================================================

void USBHostDriver::parseDescriptors(const uint8_t* descriptors, uint32_t len) {
    const uint8_t* p = descriptors;
    const uint8_t* end = descriptors + len;
    
    Serial4.println("S: Parsing descriptors...");
    
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
                
                Serial4.print("S: Found interface ");
                Serial4.print(iface->interface_num);
                Serial4.print(", class: 0x");
                Serial4.print(iface->interface_class, HEX);
                
                if (iface->is_hid) {
                    Serial4.print(" (HID");
                    if (iface->interface_protocol == 1) Serial4.print(" Keyboard");
                    else if (iface->interface_protocol == 2) Serial4.print(" Mouse");
                    Serial4.print(")");
                }
                Serial4.println();
                
                interface_count++;
            }
        }
        
        // Look for HID class descriptor
        if (desc_type == 0x21 && desc_len >= 9 && current_interface_idx < interface_count) {
            uint16_t report_length = p[7] | (p[8] << 8);
            interfaces[current_interface_idx].hid_desc_length = report_length;
            
            Serial4.print("S: HID descriptor found! Report length: ");
            Serial4.print(report_length);
            Serial4.print(" for interface ");
            Serial4.println(interfaces[current_interface_idx].interface_num);
        }
        
        // Look for endpoint descriptors
        if (desc_type == 0x05 && desc_len >= 7) { // Endpoint descriptor
            uint8_t ep_addr = p[2];
            uint8_t ep_attr = p[3];
            uint16_t ep_size = p[4] | (p[5] << 8);
            uint8_t ep_interval = p[6];
            
            Serial4.print("S: Found endpoint: addr=0x");
            Serial4.print(ep_addr, HEX);
            Serial4.print(" attr=0x");
            Serial4.print(ep_attr, HEX);
            Serial4.print(" size=");
            Serial4.print(ep_size);
            Serial4.print(" interval=");
            Serial4.println(ep_interval);
            
            // Check if it's interrupt endpoint
            if ((ep_attr & 0x03) == 0x03) {
                if (ep_addr & 0x80) {
                    // IN endpoint
                    if (!in_endpoint_addr) {
                        in_endpoint_addr = ep_addr & 0x7F;
                        in_endpoint_size = ep_size;
                        in_endpoint_interval = ep_interval;
                        Serial4.println("S: Using as primary IN endpoint");
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
                        Serial4.println("S: Using as OUT endpoint");
                    }
                }
            }
        }
        
        p += desc_len;
    }
    
    Serial4.print("S: Parsing complete - ");
    Serial4.print(interface_count);
    Serial4.print(" interfaces found, IN endpoint: ");
    Serial4.print(in_endpoint_addr);
    Serial4.print(", OUT endpoint: ");
    Serial4.println(out_endpoint_addr);
    
    // Store the number of interfaces for configuration descriptor reconstruction
    config_num_interfaces = interface_count;
}

//=============================================================================
// Endpoint Management
//=============================================================================

void USBHostDriver::claimEndpoints() {
    // Claim IN endpoint
    if (in_endpoint_addr) {
        Serial4.print("S: Creating IN pipe for endpoint ");
        Serial4.println(in_endpoint_addr);
        
        // Create interrupt pipe (type 3) for IN endpoint
        in_pipe = new_Pipe(device, 3, in_endpoint_addr, 1, in_endpoint_size, in_endpoint_interval);
        if (in_pipe) {
            in_pipe->callback_function = in_callback;
            Serial4.println("S: IN pipe created successfully");
        } else {
            Serial4.println("S: Failed to create IN pipe!");
        }
    }
    
    // Claim OUT endpoint
    if (out_endpoint_addr) {
        Serial4.print("S: Creating OUT pipe for endpoint ");
        Serial4.println(out_endpoint_addr);
        
        // Create interrupt pipe (type 3) for OUT endpoint
        out_pipe = new_Pipe(device, 3, out_endpoint_addr, 0, out_endpoint_size, out_endpoint_interval);
        if (out_pipe) {
            Serial4.println("S: OUT pipe created successfully");
        } else {
            Serial4.println("S: Failed to create OUT pipe!");
        }
    }
}

void USBHostDriver::startReading() {
    if (in_pipe && !data_transfers_paused && !pending_in_transfer) {
        Serial4.println("S: Starting data reading...");
        Serial4.print("S: IN pipe address: 0x");
        Serial4.println((uint32_t)in_pipe, HEX);
        Serial4.print("S: Buffer address: 0x");
        Serial4.println((uint32_t)rx_buffer, HEX);
        Serial4.print("S: Endpoint size: ");
        Serial4.println(in_endpoint_size);
        
        pending_in_transfer = true;
        queue_Data_Transfer(in_pipe, rx_buffer, in_endpoint_size, this);
        Serial4.println("S: Data transfer queued");
        
        Serial4.print("S: Pipe callback function: 0x");
        Serial4.println((uint32_t)in_pipe->callback_function, HEX);
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
        Serial4.println("S: Control transfer failed - no device");
        return false;
    }
    
    // Always print debug for descriptor requests
    Serial4.print("S: Control transfer: bmRequestType=0x");
    Serial4.print(bmRequestType, HEX);
    Serial4.print(" bRequest=0x");
    Serial4.print(bRequest, HEX);
    Serial4.print(" wValue=0x");
    Serial4.print(wValue, HEX);
    Serial4.print(" wIndex=0x");
    Serial4.print(wIndex, HEX);
    Serial4.print(" wLength=");
    Serial4.println(wLength);
    
    // NOTE: pauseDataTransfers() now handles waiting for pending transfers
    
    // Small delay to let device stabilize after stopping interrupt transfers
    delay(5);
    
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
    
    // Queue the transfer
    Serial4.print("S: Queuing control transfer");
    bool queue_result = queue_Control_Transfer(device, &control_setup, control_buffer, this);
    Serial4.println(queue_result ? "...success" : "...failed");
    
    // Wait for completion
    uint32_t start = millis();
    while (!control_complete && (millis() - start) < timeout_ms) {
        // Process USB tasks while waiting
        if (usbHost) {
            usbHost->Task();
        }
        yield();
    }
    
    // Check completion status
    if (!control_complete) {
        Serial4.println("S: Control transfer timeout");
        control_pending = false;  // Reset state
        return false;
    }
    
    if (!control_success) {
        Serial4.print("S: Control transfer failed, token=0x");
        Serial4.println(control_last_token, HEX);
        return false;
    }
    
    if (control_length_received == 0 && wLength > 0) {
        Serial4.println("S: Control transfer returned no data");
        return false;
    }
    
    // Success - copy data
    Serial4.print("S: Control transfer complete, received ");
    Serial4.print(control_length_received);
    Serial4.println(" bytes");
    
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
    bool debug_enabled = SunBoxStartup::isDebugEnabled();
    
    // Small delay to ensure previous Serial4 operations complete
    delayMicroseconds(100);
    
    Serial4.println("S: control() callback called");
    
    if (control_pending && transfer->buffer == control_buffer) {
        control_pending = false;
        control_complete = true;
        control_length_received = transfer->length;
        
        // Check for errors in the transfer
        uint32_t token = transfer->qtd.token;
        control_last_token = token;
        control_success = ((token >> 0) & 0xFF) == 0;  // Status byte should be 0 for success
        
        Serial4.print("S: Control transfer completed with token=0x");
        Serial4.print(token, HEX);
        Serial4.print(", length=");
        Serial4.println(control_length_received);
        
        if (!control_success) {
            Serial4.print("S: Control transfer error, token=0x");
            Serial4.println(token, HEX);
            
            if (debug_enabled) {
                Serial4.print("I: Status=");
                Serial4.print((token >> 0) & 0xFF);
                Serial4.print(", PID=");
                Serial4.print((token >> 8) & 0x03);
                Serial4.print(", Error=");
                Serial4.print((token >> 10) & 0x03);
                Serial4.print(", Active=");
                Serial4.println((token >> 7) & 0x01);
            }
        }
    } else {
        Serial4.println("S: control() called but not for our transfer");
    }
}

//=============================================================================
// Transfer Control Methods
//=============================================================================

void USBHostDriver::pauseDataTransfers() { 
    bool debug_enabled = SunBoxStartup::isDebugEnabled();
    
    if (data_transfers_paused) {
        if (debug_enabled) Serial4.println("I: Already paused");
        return;
    }
    
    data_transfers_paused = true;
    if (debug_enabled) Serial4.println("I: Data transfers paused");
    
    // Wait for any pending transfer to complete without queuing a new one
    if (pending_in_transfer) {
        if (debug_enabled) Serial4.println("I: Waiting for current transfer to complete...");
        uint32_t wait_start = millis();
        
        // Wait up to 100ms for the transfer to complete naturally
        while (pending_in_transfer && (millis() - wait_start) < 100) {
            if (usbHost) {
                usbHost->Task();
            }
            yield();
            delay(2);
        }
        
        if (!pending_in_transfer) {
            if (debug_enabled) Serial4.println("I: Current transfer completed");
        } else {
            if (debug_enabled) Serial4.println("I: Transfer still pending after wait");
            // Force clear the flag since we've waited long enough
            pending_in_transfer = false;
        }
    }
    
    // Small stabilization delay
    if (debug_enabled) Serial4.println("I: Stabilization delay...");
    delay(50);  // Reduced from 100ms to 50ms
}

void USBHostDriver::resumeDataTransfers() { 
    bool debug_enabled = SunBoxStartup::isDebugEnabled();
    
    data_transfers_paused = false; 
    if (debug_enabled) Serial4.println("I: Data transfers resumed");
    
    // Immediately queue a new transfer if we have a pipe and device is ready
    if (in_pipe && device_ready && !pending_in_transfer) {
        pending_in_transfer = true;
        queue_Data_Transfer(in_pipe, rx_buffer, in_endpoint_size, this);
        if (debug_enabled) Serial4.println("I: Queued new transfer after resume");
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
    bool debug_enabled = SunBoxStartup::isDebugEnabled();
    
    // Clear pending flag first
    pending_in_transfer = false;
    
    if (!transfer || !transfer->buffer) {
        if (debug_enabled) Serial4.println("I: Invalid transfer or buffer!");
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
            Serial4.print("S: First data packet received! Length: ");
            Serial4.print(actual_len);
            Serial4.print(" bytes: ");
            for (uint32_t i = 0; i < actual_len && i < 8; i++) {
                if (i > 0) Serial4.print(" ");
                if (((uint8_t*)transfer->buffer)[i] < 0x10) Serial4.print("0");
                Serial4.print(((uint8_t*)transfer->buffer)[i], HEX);
            }
            Serial4.println();
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
    
    // DEBUG: Log transfer state when mouse data is received - ONLY IF DEBUG ENABLED
    static uint32_t data_packet_count = 0;
    data_packet_count++;
    
    if (debug_enabled) {
        // Only log every 100 packets to reduce spam
        if ((data_packet_count % 100) == 1) {
            Serial4.print("D: Mouse packet #");
            Serial4.print(data_packet_count);
            Serial4.print(" len=");
            Serial4.print(actual_len);
            Serial4.print(" available=");
            Serial4.print(new_data_available ? "true" : "false");
            Serial4.print(" proxy_configured=");
            Serial4.println("true");  // We'll update this when proxy is integrated
        }
        
        // Always log the detailed state for the first few packets
        if (data_packet_count <= 5) {
            Serial4.print("D: processInData #");
            Serial4.print(data_packet_count);
            Serial4.print(" paused=");
            Serial4.print(data_transfers_paused ? "true" : "false");
            Serial4.print(" ready=");
            Serial4.print(device_ready ? "true" : "false");
            Serial4.print(" pipe=");
            Serial4.println(in_pipe ? "valid" : "null");
        }
    }
    
    // IMPORTANT: Queue next transfer ONLY if not paused
    if (in_pipe && !data_transfers_paused && device_ready) {
        pending_in_transfer = true;
        queue_Data_Transfer(in_pipe, rx_buffer, in_endpoint_size, this);
        
        if (debug_enabled && data_packet_count <= 5) {
            Serial4.println("D: Queued next transfer");
        }
    } else if (data_transfers_paused && debug_enabled) {
        Serial4.println("D: Not queuing - transfers paused!");
    } else if (debug_enabled) {
        Serial4.print("D: Not queuing - pipe=");
        Serial4.print(in_pipe ? "valid" : "null");
        Serial4.print(" ready=");
        Serial4.println(device_ready ? "true" : "false");
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
        Serial4.println("S: No device connected, defaulting EP0 size to 64 bytes");
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
        Serial4.println("W: Failed to get device descriptor for EP0 size - defaulting to 64");
        return 64;
    }
    
    // Extract bMaxPacketSize0 from offset 7
    uint8_t size = descriptor[7];
    
    // Sanity check - EP0 can only be 8, 16, 32, or 64 bytes per USB spec
    if (size != 8 && size != 16 && size != 32 && size != 64) {
        Serial4.print("W: Invalid EP0 size ");
        Serial4.print(size);
        Serial4.println(" bytes from device - defaulting to 64");
        return 64;
    }
    
    Serial4.print("S: Device EP0 max packet size: ");
    Serial4.print(size);
    Serial4.println(" bytes");
    
    return size;
}

// Get the actual device speed (Low/Full/High)
uint8_t USBHostDriver::getDeviceSpeed() const {
    if (!device || !device_ready) {
        Serial4.println("S: No device connected, defaulting to High Speed");
        return 2; // Default to high speed if no device
    }
    
    // CRITICAL: Convert USBHost_t36 encoding to standard encoding
    // Library: 0=Full, 1=Low, 2=High
    // We need: 0=Low, 1=Full, 2=High
    
    uint8_t lib_speed = device->speed;
    uint8_t standard_speed;
    
    switch (lib_speed) {
        case 0:  // Library says Full Speed (12 Mbps)
            standard_speed = 1;
            Serial4.println("S: Full Speed (12 Mbps)");
            break;
        case 1:  // Library says Low Speed (1.5 Mbps)
            standard_speed = 0;
            Serial4.println("S: Low Speed (1.5 Mbps)");
            break;
        case 2:  // Library says High Speed (480 Mbps)
            standard_speed = 2;
            Serial4.println("S: High Speed (480 Mbps)");
            break;
        default:
            Serial4.print("S: Unknown speed encoding: ");
            Serial4.println(lib_speed);
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
    Serial4.println("\nI: === USB Device Descriptor Dump ===");
    
    if (!device) {
        Serial4.println("I: No device connected!");
        return;
    }
    
    // Device info
    Serial4.print("I: Device VID: 0x");
    if (device->idVendor < 0x10) Serial4.print("0");
    if (device->idVendor < 0x100) Serial4.print("0");
    if (device->idVendor < 0x1000) Serial4.print("0");
    Serial4.print(device->idVendor, HEX);
    Serial4.print(" PID: 0x");
    if (device->idProduct < 0x10) Serial4.print("0");
    if (device->idProduct < 0x100) Serial4.print("0");
    if (device->idProduct < 0x1000) Serial4.print("0");
    Serial4.println(device->idProduct, HEX);
    
    Serial4.print("I: Device Class: 0x");
    if (device->bDeviceClass < 0x10) Serial4.print("0");
    Serial4.println(device->bDeviceClass, HEX);
    
    // Interface summary
    Serial4.print("\nI: Total Interfaces: ");
    Serial4.println(interface_count);
    
    // If we have stored configuration descriptor, parse it for display
    if (config_descriptor_valid && config_descriptor_len > 0) {
        Serial4.println("\nI: [Using stored configuration descriptor]");
        displayStoredDescriptors();
    } else {
        // Fall back to basic interface info from our structures
        Serial4.println("\nI: [No stored descriptors available - showing parsed data only]");
        
        // Dump each interface
        for (uint8_t i = 0; i < interface_count; i++) {
            Serial4.print("\nI: --- Interface ");
            Serial4.print(interfaces[i].interface_num);
            Serial4.println(" ---");
            
            Serial4.print("I:   Class: 0x");
            if (interfaces[i].interface_class < 0x10) Serial4.print("0");
            Serial4.print(interfaces[i].interface_class, HEX);
            
            if (interfaces[i].is_hid) {
                Serial4.print(" (HID");
                switch (interfaces[i].interface_protocol) {
                    case 0: Serial4.print(" - None"); break;
                    case 1: Serial4.print(" - Keyboard"); break;
                    case 2: Serial4.print(" - Mouse"); break;
                    default: Serial4.print(" - Other"); break;
                }
                Serial4.print(")");
            }
            Serial4.println();
            
            Serial4.print("I:   Subclass: 0x");
            if (interfaces[i].interface_subclass < 0x10) Serial4.print("0");
            Serial4.println(interfaces[i].interface_subclass, HEX);
            
            Serial4.print("I:   Protocol: 0x");
            if (interfaces[i].interface_protocol < 0x10) Serial4.print("0");
            Serial4.println(interfaces[i].interface_protocol, HEX);
            
            if (interfaces[i].is_hid && interfaces[i].hid_desc_length > 0) {
                Serial4.print("I:   HID Descriptor Length: ");
                Serial4.print(interfaces[i].hid_desc_length);
                Serial4.println(" bytes");
            }
            
            if (interfaces[i].has_in_endpoint) {
                Serial4.print("I:   IN Endpoint: 0x");
                if ((interfaces[i].in_endpoint_addr | 0x80) < 0x10) Serial4.print("0");
                Serial4.print(interfaces[i].in_endpoint_addr | 0x80, HEX);
                Serial4.print(" (");
                Serial4.print(interfaces[i].in_endpoint_size);
                Serial4.print(" bytes, interval ");
                Serial4.print(interfaces[i].in_endpoint_interval);
                Serial4.println("ms)");
            } else {
                Serial4.println("I:   No IN endpoint");
            }
        }
    }
    
    // Current configuration
    Serial4.println("\nI: --- Current Configuration ---");
    Serial4.print("I: Primary IN Endpoint: ");
    if (in_endpoint_addr) {
        Serial4.print("0x");
        if ((in_endpoint_addr | 0x80) < 0x10) Serial4.print("0");
        Serial4.print(in_endpoint_addr | 0x80, HEX);
        Serial4.print(" (");
        Serial4.print(in_endpoint_size);
        Serial4.print(" bytes)");
    } else {
        Serial4.print("None");
    }
    Serial4.println();
    
    Serial4.print("I: Primary OUT Endpoint: ");
    if (out_endpoint_addr) {
        Serial4.print("0x");
        if (out_endpoint_addr < 0x10) Serial4.print("0");
        Serial4.print(out_endpoint_addr, HEX);
        Serial4.print(" (");
        Serial4.print(out_endpoint_size);
        Serial4.print(" bytes)");
    } else {
        Serial4.print("None");
    }
    Serial4.println();
    
    Serial4.println("\nI: =================================");
}

void USBHostDriver::displayStoredDescriptors() {
    bool debug_enabled = SunBoxStartup::isDebugEnabled();
    
    // First, display the reconstructed Configuration Descriptor header
    Serial4.println("\nI: --- Configuration Descriptor (Reconstructed) ---");
    Serial4.print("I:   Total Length: ");
    Serial4.print(config_descriptor_len + 9);  // Add 9 for config descriptor header
    Serial4.println(" bytes");
    Serial4.print("I:   Number of Interfaces: ");
    Serial4.println(config_num_interfaces);
    Serial4.print("I:   Configuration Value: ");
    Serial4.println(config_value);
    Serial4.println("I:   Configuration String: 0");  // We don't store this
    Serial4.print("I:   Attributes: 0x");
    Serial4.print(config_attributes, HEX);
    Serial4.print(" (");
    if (config_attributes & 0x80) Serial4.print("Bus Powered ");
    if (config_attributes & 0x40) Serial4.print("Self Powered ");
    if (config_attributes & 0x20) Serial4.print("Remote Wakeup ");
    Serial4.println(")");
    Serial4.print("I:   Max Power: ");
    Serial4.print(config_max_power * 2);
    Serial4.println(" mA");
    
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
            Serial4.print("\nI: --- Interface ");
            Serial4.print(current_interface);
            Serial4.println(" ---");
            Serial4.print("I:   Alternate Setting: ");
            Serial4.println(p[3]);
            Serial4.print("I:   Number of Endpoints: ");
            Serial4.println(p[4]);
            Serial4.print("I:   Class: 0x");
            if (p[5] < 0x10) Serial4.print("0");
            Serial4.print(p[5], HEX);
            
            if (p[5] == 0x03) {
                Serial4.print(" (HID");
                switch (p[7]) {
                    case 0: Serial4.print(" - None"); break;
                    case 1: Serial4.print(" - Keyboard"); break;
                    case 2: Serial4.print(" - Mouse"); break;
                    default: Serial4.print(" - Other"); break;
                }
                Serial4.print(")");
            }
            Serial4.println();
            
            Serial4.print("I:   Subclass: 0x");
            if (p[6] < 0x10) Serial4.print("0");
            Serial4.println(p[6], HEX);
            
            Serial4.print("I:   Protocol: 0x");
            if (p[7] < 0x10) Serial4.print("0");
            Serial4.println(p[7], HEX);
            
            Serial4.print("I:   Interface String: ");
            Serial4.println(p[8]);
        }
        
        // HID class descriptor
        else if (desc_type == 0x21 && desc_len >= 9) {
            Serial4.println("I:   HID Class Descriptor:");
            Serial4.print("I:     HID Version: ");
            Serial4.print(p[3]);
            Serial4.print(".");
            Serial4.println(p[2]);
            Serial4.print("I:     Country Code: ");
            Serial4.println(p[4]);
            Serial4.print("I:     Number of Descriptors: ");
            Serial4.println(p[5]);
            
            // Parse descriptor info
            for (uint8_t i = 0; i < p[5] && (6 + i*3 + 2) < desc_len; i++) {
                uint8_t dtype = p[6 + i*3];
                uint16_t dlen = p[7 + i*3] | (p[8 + i*3] << 8);
                Serial4.print("I:     Descriptor ");
                Serial4.print(i);
                Serial4.print(": Type=0x");
                Serial4.print(dtype, HEX);
                if (dtype == 0x22) Serial4.print(" (Report)");
                else if (dtype == 0x23) Serial4.print(" (Physical)");
                Serial4.print(", Length=");
                Serial4.print(dlen);
                Serial4.println(" bytes");
            }
        }
        
        // Endpoint descriptor
        else if (desc_type == 0x05 && desc_len >= 7) {
            Serial4.print("I:   Endpoint 0x");
            if (p[2] < 0x10) Serial4.print("0");
            Serial4.print(p[2], HEX);
            Serial4.print(" (");
            if (p[2] & 0x80) Serial4.print("IN"); else Serial4.print("OUT");
            Serial4.println("):");
            
            Serial4.print("I:     Attributes: 0x");
            if (p[3] < 0x10) Serial4.print("0");
            Serial4.print(p[3], HEX);
            Serial4.print(" (");
            switch (p[3] & 0x03) {
                case 0: Serial4.print("Control"); break;
                case 1: Serial4.print("Isochronous"); break;
                case 2: Serial4.print("Bulk"); break;
                case 3: Serial4.print("Interrupt"); break;
            }
            Serial4.println(")");
            
            Serial4.print("I:     Max Packet Size: ");
            Serial4.print(p[4] | (p[5] << 8));
            Serial4.println(" bytes");
            
            Serial4.print("I:     Interval: ");
            Serial4.print(p[6]);
            Serial4.println(" ms");
        }
        
        // Unknown descriptor
        else {
            Serial4.print("\nI:   Unknown Descriptor Type 0x");
            if (desc_type < 0x10) Serial4.print("0");
            Serial4.print(desc_type, HEX);
            Serial4.print(", Length=");
            Serial4.println(desc_len);
        }
        
        p += desc_len;
    }
    
    // Display raw hex dump at the end - now with reconstructed header
    if (debug_enabled) {
        Serial4.println("\nI: --- Raw Configuration Descriptor Hex Dump ---");
        Serial4.println("I:   [Reconstructed header + stored descriptors]");
        
        // First show the reconstructed configuration descriptor header
        Serial4.print("I:   000: 09 02 ");
        uint16_t total_len = config_descriptor_len + 9;
        if ((total_len & 0xFF) < 0x10) Serial4.print("0");
        Serial4.print(total_len & 0xFF, HEX);
        Serial4.print(" ");
        if (((total_len >> 8) & 0xFF) < 0x10) Serial4.print("0");
        Serial4.print((total_len >> 8) & 0xFF, HEX);
        Serial4.print(" ");
        if (config_num_interfaces < 0x10) Serial4.print("0");
        Serial4.print(config_num_interfaces, HEX);
        Serial4.print(" ");
        if (config_value < 0x10) Serial4.print("0");
        Serial4.print(config_value, HEX);
        Serial4.print(" 00 ");  // Config string index
        Serial4.print(config_attributes, HEX);
        Serial4.print(" ");
        Serial4.print(config_max_power, HEX);
        Serial4.print("                  ");  // Padding
        Serial4.println(" [Config Header]");
        
        // Then show the stored descriptors
        for (uint16_t i = 0; i < config_descriptor_len; i += 16) {
            Serial4.print("I:   ");
            uint16_t addr = i + 9;  // Account for header
            if (addr < 0x10) Serial4.print("0");
            if (addr < 0x100) Serial4.print("0");
            Serial4.print(addr, HEX);
            Serial4.print(": ");
            
            // Hex bytes
            for (uint16_t j = 0; j < 16 && (i + j) < config_descriptor_len; j++) {
                if (config_descriptor[i + j] < 0x10) Serial4.print("0");
                Serial4.print(config_descriptor[i + j], HEX);
                Serial4.print(" ");
            }
            
            // Padding if last line
            if (config_descriptor_len - i < 16) {
                for (uint16_t j = config_descriptor_len - i; j < 16; j++) {
                    Serial4.print("   ");
                }
            }
            
            Serial4.print(" ");
            
            // ASCII representation
            for (uint16_t j = 0; j < 16 && (i + j) < config_descriptor_len; j++) {
                char c = config_descriptor[i + j];
                if (c >= ' ' && c <= '~') {
                    Serial4.print(c);
                } else {
                    Serial4.print(".");
                }
            }
            
            // Padding if last line
            if (config_descriptor_len - i < 16) {
                for (uint16_t j = config_descriptor_len - i; j < 16; j++) {
                    Serial4.print("   ");
                }
            }
            
            Serial4.print(" ");
            
            // ASCII representation
            for (uint16_t j = 0; j < 16 && (i + j) < config_descriptor_len; j++) {
                char c = config_descriptor[i + j];
                if (c >= ' ' && c <= '~') {
                    Serial4.print(c);
                } else {
                    Serial4.print(".");
                }
            }
            
            Serial4.println();
        }
        
        Serial4.print("I: Total bytes: ");
        Serial4.print(total_len);
        Serial4.print(" (9 header + ");
        Serial4.print(config_descriptor_len);
        Serial4.println(" descriptors)");
    }
}