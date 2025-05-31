// USBHostDriver.cpp
#include "USBHostDriver.h"

USBHostDriver::USBHostDriver(USBHost& host) 
    : usbHost(&host), device(nullptr), device_ready(false), device_claimed(false),
      strings_fetched(false), connect_time(0), interface_count(0),
      control_pending(false), control_complete(false), control_length_received(0),
      control_success(false), in_pipe(nullptr), out_pipe(nullptr),
      in_endpoint_addr(0), out_endpoint_addr(0), in_endpoint_size(0),
      out_endpoint_size(0), in_endpoint_interval(1), out_endpoint_interval(1),
      last_rx_length(0), new_data_available(false),
      data_callback(nullptr), rx_transfer_index(0) {
    
    Serial4.println("[STARTUP]: USBHostDriver constructor called");
    
    // Initialize strings
    manufacturer_string[0] = '\0';
    product_string[0] = '\0';
    serial_string[0] = '\0';
    
    // Initialize buffers
    memset(rx_buffer, 0, sizeof(rx_buffer));
    memset(last_rx_buffer, 0, sizeof(last_rx_buffer));
    memset(control_buffer, 0, sizeof(control_buffer));
    
    // Clear all pipes and transfers
    memset(mypipes, 0, sizeof(mypipes));
    memset(mytransfers, 0, sizeof(mytransfers));
    
    // Clear interface info
    memset(interfaces, 0, sizeof(interfaces));
    
    // Contribute resources to USB Host - CRITICAL!
    Serial4.println("[STARTUP]: Contributing Pipes and Transfers to USB Host");
    contribute_Pipes(mypipes, sizeof(mypipes)/sizeof(Pipe_t));
    contribute_Transfers(mytransfers, sizeof(mytransfers)/sizeof(Transfer_t));
    
    // Register with USB Host BEFORE it starts - CRITICAL!
    Serial4.println("[STARTUP]: Registering driver with USB Host (in constructor)");
    driver_ready_for_device(this);
    
    Serial4.println("[STARTUP]: USBHostDriver constructor complete");
}

USBHostDriver::~USBHostDriver() {
    Serial4.println("[STARTUP]: USBHostDriver destructor called - THIS SHOULD NOT HAPPEN!");
    if (device_claimed) {
        disconnect();
    }
}

bool USBHostDriver::begin() {
    Serial4.println("[STARTUP]: SunBox Host Driver begin() called.");
    Serial4.println("[STARTUP]: SunBox Host Driver ready to receive devices.");
    return true;
}

bool USBHostDriver::claim(Device_t *dev, int type, const uint8_t *descriptors, uint32_t len) {
    Serial4.println();
    Serial4.print("[STARTUP]: USBHostDriver::claim() called - type: ");
    Serial4.print(type);
    Serial4.print(", len: ");
    Serial4.println(len);
    
    // Type 0 = Device level (store reference but don't claim)
    if (type == CLAIM_REPORT) {
        Serial4.println("[STARTUP]: Device level claim (type 0) - storing device reference");
        device = dev;
        Serial4.print("[STARTUP]: Device VID: 0x");
        Serial4.print(dev->idVendor, HEX);
        Serial4.print(", PID: 0x");
        Serial4.println(dev->idProduct, HEX);
        return false;  // Let enumeration continue
    }
    
    // Type 1 = Interface level (actual claiming)
    if (type == CLAIM_INTERFACE) {
        if (device_claimed) {
            Serial4.println("[STARTUP]: Already have a claimed device, skipping");
            return false;
        }
        
        if (!dev || dev != device) {
            Serial4.println("[STARTUP]: Invalid device pointer or mismatch");
            return false;
        }
        
        Serial4.println("[STARTUP]: Interface level claim (type 1) - claiming device!");
        Serial4.println("[STARTUP]: SunBox Host Driver beginning claim process...");
        
        Serial4.print("[STARTUP]: Device VID: 0x");
        Serial4.print(dev->idVendor, HEX);
        Serial4.print(", PID: 0x");
        Serial4.println(dev->idProduct, HEX);
        
        Serial4.print("[STARTUP]: Descriptors length: ");
        Serial4.println(len);
        
        connect_time = millis();
        
        // Parse descriptors to find HID interfaces and endpoints
        if (len > 0 && descriptors != nullptr) {
            parseDescriptors(descriptors, len);
        } else {
            Serial4.println("[STARTUP]: No descriptors provided, using defaults");
            // Set some default endpoints for testing
            in_endpoint_addr = 1;
            in_endpoint_size = 64;
            in_endpoint_interval = 1;
        }
        
        // Claim the endpoints
        claimEndpoints();
        
        // For now, we'll use generic strings
        strcpy(manufacturer_string, "Unknown");
        strcpy(product_string, "USB Device");
        strcpy(serial_string, "No Serial");
        
        // Log device information
        Serial4.print("[STARTUP]: SunBox Host Driver found device (");
        Serial4.print(manufacturer_string);
        Serial4.print(" : ");
        Serial4.print(product_string);
        Serial4.print(" : ");
        Serial4.print(serial_string);
        Serial4.println(")");
        
        device_claimed = true;
        device_ready = true;
        
        // Start reading data
        startReading();
        
        Serial4.println("[STARTUP]: SunBox Host Driver claim process complete.");
        Serial4.println("[STARTUP]: Device successfully claimed!");
        
        return true;  // Claim this interface
    }
    
    Serial4.print("[STARTUP]: Unknown claim type: ");
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
    strings_fetched = false;
    interface_count = 0;
    
    // Clear endpoint info
    in_endpoint_addr = 0;
    out_endpoint_addr = 0;
    in_endpoint_size = 0;
    out_endpoint_size = 0;
    in_endpoint_interval = 1;
    out_endpoint_interval = 1;
    
    Serial4.println("[STARTUP]: SunBox Host Driver device disconnected.");
}

void USBHostDriver::parseDescriptors(const uint8_t* descriptors, uint32_t len) {
    const uint8_t* p = descriptors;
    const uint8_t* end = descriptors + len;
    
    Serial4.println("[STARTUP]: Parsing descriptors...");
    
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
                
                Serial4.print("[STARTUP]: Found interface ");
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
            // HID descriptor format:
            // [0] bLength
            // [1] bDescriptorType (0x21)
            // [2-3] bcdHID
            // [4] bCountryCode
            // [5] bNumDescriptors
            // [6] bDescriptorType (0x22 = Report)
            // [7-8] wDescriptorLength (little-endian)
            
            uint16_t report_length = p[7] | (p[8] << 8);
            interfaces[current_interface_idx].hid_desc_length = report_length;
            
            Serial4.print("[STARTUP]: HID descriptor found! Report length: ");
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
            
            Serial4.print("[STARTUP]: Found endpoint: addr=0x");
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
                        Serial4.println("[STARTUP]: Using as primary IN endpoint");
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
                        Serial4.println("[STARTUP]: Using as OUT endpoint");
                    }
                }
            }
        }
        
        p += desc_len;
    }
    
    Serial4.print("[STARTUP]: Parsing complete - ");
    Serial4.print(interface_count);
    Serial4.print(" interfaces found, IN endpoint: ");
    Serial4.print(in_endpoint_addr);
    Serial4.print(", OUT endpoint: ");
    Serial4.println(out_endpoint_addr);
}

void USBHostDriver::claimEndpoints() {
    // Claim IN endpoint
    if (in_endpoint_addr) {
        Serial4.print("[STARTUP]: Creating IN pipe for endpoint ");
        Serial4.println(in_endpoint_addr);
        
        // Create interrupt pipe (type 3) for IN endpoint
        in_pipe = new_Pipe(device, 3, in_endpoint_addr, 1, in_endpoint_size, in_endpoint_interval);
        if (in_pipe) {
            in_pipe->callback_function = in_callback;
            Serial4.println("[STARTUP]: IN pipe created successfully");
        } else {
            Serial4.println("[STARTUP]: Failed to create IN pipe!");
        }
    }
    
    // Claim OUT endpoint
    if (out_endpoint_addr) {
        Serial4.print("[STARTUP]: Creating OUT pipe for endpoint ");
        Serial4.println(out_endpoint_addr);
        
        // Create interrupt pipe (type 3) for OUT endpoint
        out_pipe = new_Pipe(device, 3, out_endpoint_addr, 0, out_endpoint_size, out_endpoint_interval);
        if (out_pipe) {
            Serial4.println("[STARTUP]: OUT pipe created successfully");
        } else {
            Serial4.println("[STARTUP]: Failed to create OUT pipe!");
        }
    }
}

void USBHostDriver::startReading() {
    if (in_pipe) {
        Serial4.println("[STARTUP]: Starting data reading...");
        queue_Data_Transfer(in_pipe, rx_buffer, in_endpoint_size, this);
        Serial4.println("[STARTUP]: Data transfer queued");
    }
}

// Control transfer implementation
bool USBHostDriver::controlTransfer(uint8_t bmRequestType, uint8_t bRequest, 
                                   uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                                   uint8_t* data, uint16_t* actualLength, 
                                   uint32_t timeout_ms) {
    if (!device || !device_claimed) {
        Serial4.println("[DRIVER]: Control transfer failed - no device");
        return false;
    }
    
    Serial4.print("[DRIVER]: Control transfer: bmRequestType=0x");
    Serial4.print(bmRequestType, HEX);
    Serial4.print(" bRequest=0x");
    Serial4.print(bRequest, HEX);
    Serial4.print(" wValue=0x");
    Serial4.print(wValue, HEX);
    Serial4.print(" wIndex=0x");
    Serial4.print(wIndex, HEX);
    Serial4.print(" wLength=");
    Serial4.println(wLength);
    
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
    
    // Queue the transfer
    queue_Control_Transfer(device, &control_setup, control_buffer, this);
    
    // Wait for completion
    uint32_t start = millis();
    while (!control_complete && (millis() - start) < timeout_ms) {
        delay(1);
    }
    
    if (control_complete && control_success && control_length_received > 0) {
        Serial4.print("[DRIVER]: Control transfer complete, received ");
        Serial4.print(control_length_received);
        Serial4.println(" bytes");
        
        // Copy data to caller's buffer
        if (data) {
            uint16_t copy_len = min(control_length_received, wLength);
            memcpy(data, control_buffer, copy_len);
            if (actualLength) *actualLength = copy_len;
        }
        return true;
    }
    
    if (!control_complete) {
        Serial4.println("[DRIVER]: Control transfer timeout");
    } else if (!control_success) {
        Serial4.println("[DRIVER]: Control transfer failed");
    } else {
        Serial4.println("[DRIVER]: Control transfer returned no data");
    }
    return false;
}

void USBHostDriver::control(const Transfer_t *transfer) {
    if (control_pending && transfer->buffer == control_buffer) {
        control_pending = false;
        control_complete = true;
        control_length_received = transfer->length;
        
        // Check for errors in the transfer
        uint32_t token = transfer->qtd.token;
        control_success = ((token >> 0) & 0xFF) == 0;  // Status byte should be 0 for success
        
        if (!control_success) {
            Serial4.print("[DRIVER]: Control transfer error, token=0x");
            Serial4.println(token, HEX);
        }
    }
}

// Interface information methods
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

// Data callbacks and transfer methods remain the same
void USBHostDriver::in_callback(const Transfer_t *transfer) {
    if (!transfer) return;
    
    USBHostDriver* driver = (USBHostDriver*)transfer->driver;
    if (driver) {
        driver->processInData(transfer);
    }
}

void USBHostDriver::processInData(const Transfer_t *transfer) {
    if (!transfer || !transfer->buffer) return;
    
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
            Serial4.print("[STARTUP]: First data packet received! Length: ");
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
    
    // Queue next transfer
    if (in_pipe) {
        queue_Data_Transfer(in_pipe, rx_buffer, in_endpoint_size, this);
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