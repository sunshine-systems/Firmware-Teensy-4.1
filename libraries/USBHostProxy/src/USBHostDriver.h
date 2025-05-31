// USBHostDriver.h
#ifndef _USBHOSTDRIVER_H_
#define _USBHOSTDRIVER_H_

#include <Arduino.h>
#include <USBHost_t36.h>

#define MAX_ENDPOINTS 4
#define RX_BUFFER_SIZE 64
#define MAX_INTERFACES 8

// Claim types from USBHost_t36
#define CLAIM_REPORT        0
#define CLAIM_INTERFACE     1

// Data callback function type
typedef void (*data_callback_t)(const uint8_t* data, uint32_t length);

class USBHostDriver : public USBDriver {
public:
    USBHostDriver(USBHost& host);
    ~USBHostDriver();
    
    // Startup and status
    bool begin();
    bool isReady() const { return device_ready; }
    
    // Device information
    uint16_t getVendorID() const { return device ? device->idVendor : 0; }
    uint16_t getProductID() const { return device ? device->idProduct : 0; }
    const char* getManufacturer() const { return manufacturer_string; }
    const char* getProduct() const { return product_string; }
    const char* getSerial() const { return serial_string; }
    
    // Control transfer API
    bool controlTransfer(uint8_t bmRequestType, uint8_t bRequest, 
                        uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                        uint8_t* data, uint16_t* actualLength, 
                        uint32_t timeout_ms = 100);
    
    // Interface information
    uint8_t getInterfaceCount() const { return interface_count; }
    uint8_t getInterfaceNumber(uint8_t index) const;
    uint8_t getInterfaceClass(uint8_t index) const;
    uint8_t getInterfaceSubclass(uint8_t index) const;
    uint8_t getInterfaceProtocol(uint8_t index) const;
    uint16_t getHIDDescriptorLength(uint8_t interface_index) const;
    bool isHIDInterface(uint8_t interface_index) const;
    
    // Find interface by criteria
    int8_t findInterface(uint8_t class_code, uint8_t subclass = 0xFF, uint8_t protocol = 0xFF) const;
    
    // Endpoint information for an interface
    uint8_t getEndpointAddress(uint8_t interface_index) const;
    uint16_t getEndpointSize(uint8_t interface_index) const;
    uint8_t getEndpointInterval(uint8_t interface_index) const;
    
    // Data transfer
    bool sendData(const uint8_t* data, uint32_t length);
    void setDataCallback(data_callback_t callback) { data_callback = callback; }
    bool getLastData(uint8_t* buffer, uint32_t& length);
    
protected:
    // USBDriver interface implementation
    virtual bool claim(Device_t *dev, int type, const uint8_t *descriptors, uint32_t len) override;
    virtual void disconnect() override;
    virtual void control(const Transfer_t *transfer) override;
    
private:
    void parseDescriptors(const uint8_t* descriptors, uint32_t len);
    void claimEndpoints();
    void startReading();
    
    // String descriptor helpers (simplified for now)
    void requestStringDescriptor(uint8_t index, char* buffer, uint8_t buflen);
    void processStringDescriptor(const Transfer_t *transfer);
    
    // Data transfer callbacks
    static void in_callback(const Transfer_t *transfer);
    void processInData(const Transfer_t *transfer);
    
    // USB Host reference
    USBHost* usbHost;
    
    // Device state
    Device_t* device;
    bool device_ready;
    bool device_claimed;
    bool strings_fetched;
    uint32_t connect_time;
    
    // Device strings
    char manufacturer_string[64];
    char product_string[64];
    char serial_string[64];
    uint8_t string_buffer[256];
    uint8_t pending_string_index;
    char* pending_string_buffer;
    
    // Interface tracking
    struct InterfaceInfo {
        uint8_t interface_num;
        uint8_t interface_class;
        uint8_t interface_subclass;
        uint8_t interface_protocol;
        uint16_t hid_desc_length;
        bool is_hid;
        // Store first IN endpoint info for each interface
        uint8_t in_endpoint_addr;
        uint16_t in_endpoint_size;
        uint8_t in_endpoint_interval;
        bool has_in_endpoint;
    } interfaces[MAX_INTERFACES];
    uint8_t interface_count;
    
    // Control transfer state
    setup_t control_setup;
    uint8_t control_buffer[512] __attribute__ ((aligned(32)));
    volatile bool control_pending;
    volatile bool control_complete;
    volatile uint16_t control_length_received;
    volatile bool control_success;
    
    // Endpoints
    Pipe_t* in_pipe;
    Pipe_t* out_pipe;
    uint8_t in_endpoint_addr;
    uint8_t out_endpoint_addr;
    uint16_t in_endpoint_size;
    uint16_t out_endpoint_size;
    uint8_t in_endpoint_interval;
    uint8_t out_endpoint_interval;
    
    // USB resources
    Pipe_t mypipes[8] __attribute__ ((aligned(32)));
    Transfer_t mytransfers[16] __attribute__ ((aligned(32)));
    
    // Data buffers
    uint8_t rx_buffer[RX_BUFFER_SIZE] __attribute__ ((aligned(32)));
    uint8_t last_rx_buffer[RX_BUFFER_SIZE];
    uint32_t last_rx_length;
    bool new_data_available;
    
    // Data callback
    data_callback_t data_callback;
    
    // Transfer tracking
    uint8_t rx_transfer_index;
};

#endif // _USBHOSTDRIVER_H_