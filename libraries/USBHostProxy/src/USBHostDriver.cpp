// USBHostDriver.cpp
#include "USBHostDriver.h"

USBHostDriver::USBHostDriver(USBHost& host) 
    : usbHost(&host), device(nullptr), device_ready(false), device_claimed(false),
      strings_fetched(false), connect_time(0), hid_descriptor_length(0),
      hid_descriptor_available(false), in_pipe(nullptr), out_pipe(nullptr),
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
    memset(hid_descriptor, 0, sizeof(hid_descriptor));
    
    // Clear all pipes and transfers
    memset(mypipes, 0, sizeof(mypipes));
    memset(mytransfers, 0, sizeof(mytransfers));
    
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
    
    // Driver is already registered in constructor
    // This method is now just for compatibility
    
    Serial4.println("[STARTUP]: SunBox Host Driver ready to receive devices.");
    
    return true;
}

bool USBHostDriver::claim(Device_t *dev, int type, const uint8_t *descriptors, uint32_t len) {
    Serial4.println();  // New line to separate from any ongoing output
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
        
        // Parse descriptors to find HID interface and endpoints
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
    
    while (p < end) {
        uint8_t desc_len = p[0];
        uint8_t desc_type = p[1];
        
        if (p + desc_len > end) break;
        
        // Look for interface descriptor
        if (desc_type == 0x04 && desc_len >= 9) { // Interface descriptor
            uint8_t interface_class = p[5];
            Serial4.print("[STARTUP]: Found interface, class: 0x");
            Serial4.println(interface_class, HEX);
            
            // Accept HID devices (class 3) or any device for testing
            if (interface_class == 0x03) {
                Serial4.println("[STARTUP]: HID interface found!");
            }
        }
        
        // Look for HID class descriptor
        if (desc_type == 0x21 && desc_len >= 9) { // HID descriptor
            hid_descriptor_available = true;
            Serial4.println("[STARTUP]: HID descriptor found!");
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
                        Serial4.println("[STARTUP]: Using as IN endpoint");
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
    
    Serial4.print("[STARTUP]: Parsing complete - IN endpoint: ");
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
        // Parameters: device, type, endpoint, direction (1=IN), maxPacketSize, interval
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
        // Parameters: device, type, endpoint, direction (0=OUT), maxPacketSize, interval
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
        // Queue initial transfer using USBHost_t36 API
        // The callback is already set on the pipe in claimEndpoints()
        queue_Data_Transfer(in_pipe, rx_buffer, in_endpoint_size, this);
        Serial4.println("[STARTUP]: Data transfer queued");
    }
}

void USBHostDriver::requestStringDescriptor(uint8_t index, char* buffer, uint8_t buflen) {
    // String descriptor support would require control transfers
    // For now, we'll skip this functionality
}

void USBHostDriver::control(const Transfer_t *transfer) {
    // Control transfer callback - not used in this simplified version
}

void USBHostDriver::processStringDescriptor(const Transfer_t *transfer) {
    // String descriptor processing - not used in this simplified version
}

bool USBHostDriver::getHIDDescriptor(const uint8_t** descriptor, uint16_t* length) {
    if (!hid_descriptor_available || hid_descriptor_length == 0) {
        // For now, return false to trigger boot protocol
        // In a full implementation, we would request the HID report descriptor here
        return false;
    }
    
    *descriptor = hid_descriptor;
    *length = hid_descriptor_length;
    return true;
}

void USBHostDriver::in_callback(const Transfer_t *transfer) {
    if (!transfer) return;
    
    // Get driver instance from transfer
    USBHostDriver* driver = (USBHostDriver*)transfer->driver;
    if (driver) {
        driver->processInData(transfer);
    }
}

void USBHostDriver::processInData(const Transfer_t *transfer) {
    if (!transfer || !transfer->buffer) return;
    
    // Calculate actual received length (like USBProxyDriver does)
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
    // Would use queue_Data_Transfer with out_pipe
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