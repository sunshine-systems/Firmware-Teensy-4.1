#ifndef DEVICE_ENUMERATOR_H
#define DEVICE_ENUMERATOR_H

#include <Arduino.h>
#include <USBHost_t36.h> 
#include <string.h>      
#include <stddef.h>      

#define ENABLE_DE_LOGGING true 

// --- Standard USB Definitions ---
#define USB_REQUEST_GET_DESCRIPTOR    6
#define USB_DESCRIPTOR_DEVICE           1
#define USB_DESCRIPTOR_STRING           3
#define USB_DESCRIPTOR_INTERFACE        4
#define USB_DESCRIPTOR_ENDPOINT         5
#define USB_DESC_TYPE_HID               0x21
#define USB_DESC_TYPE_REPORT            0x22
#define USB_SPEED_FULL  0 // Ensure these are defined
#define USB_SPEED_LOW   1
#define USB_SPEED_HIGH  2
// Add any other USB_DESCRIPTOR_ or USB_REQUEST_ constants used by your structs or dummy data

// --- Descriptor Structures (Ensure these are minimal but sufficient for UsbDeviceData) ---
typedef struct {
    uint8_t  bLength; uint8_t  bDescriptorType; uint16_t bcdUSB;
    uint8_t  bDeviceClass; uint8_t  bDeviceSubClass; uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0; uint16_t idVendor; uint16_t idProduct;
    uint16_t bcdDevice; uint8_t  iManufacturer; uint8_t  iProduct;
    uint8_t  iSerialNumber; uint8_t  bNumConfigurations;
} __attribute__((packed)) usb_device_descriptor_t;

// --- USB Data Storage Structures ---
#define MAX_STRING_LENGTH 128 
#define MAX_CONFIGURATIONS_PER_DEVICE 1
#define MAX_INTERFACES_PER_CONFIG 2 
#define MAX_ENDPOINTS_PER_INTERFACE 2 
#define MAX_HID_REPORT_DESC_PER_INTERFACE 1 

struct UsbEndpointData {
    uint8_t  bEndpointAddress; uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;   uint8_t  bInterval;
    uint8_t  bSynchAddress;
};

struct UsbHidReportDescInfo {
    uint8_t   bDescriptorType; uint16_t  wDescriptorLength;
    const uint8_t*  rawData; 
};

struct UsbInterfaceData {
    uint8_t bInterfaceNumber; uint8_t bAlternateSetting; uint8_t bNumEndpoints;
    uint8_t bInterfaceClass; uint8_t bInterfaceSubClass; uint8_t bInterfaceProtocol;
    uint8_t iInterface; char interfaceString[MAX_STRING_LENGTH];
    UsbEndpointData endpoints[MAX_ENDPOINTS_PER_INTERFACE]; uint8_t endpointCount;
    bool isHidInterface; uint16_t bcdHID; uint8_t bCountryCode;
    uint8_t bNumHidClassDescriptors;
    UsbHidReportDescInfo reportDescriptors[MAX_HID_REPORT_DESC_PER_INTERFACE];
    uint8_t reportDescriptorCount;
};

struct UsbConfigurationData {
    uint8_t  bConfigurationValue; uint8_t  iConfiguration;
    char     configurationString[MAX_STRING_LENGTH];
    uint8_t  bmAttributes; uint8_t  bMaxPower; uint16_t wTotalLength;
    UsbInterfaceData interfaces[MAX_INTERFACES_PER_CONFIG];
    uint8_t          interfaceCount;
};

struct UsbDeviceData {
    uint16_t idVendor; uint16_t idProduct; uint16_t bcdUSB;
    uint8_t  bDeviceClass; uint8_t  bDeviceSubClass; uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0; uint16_t bcdDevice;
    uint8_t  iManufacturer; uint8_t  iProduct; uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
    uint8_t  speed; uint8_t  hub_address; uint8_t  hub_port; uint8_t  address;
    char manufacturerString[MAX_STRING_LENGTH];
    char productString[MAX_STRING_LENGTH];
    char serialNumberString[MAX_STRING_LENGTH];
    UsbConfigurationData configurations[MAX_CONFIGURATIONS_PER_DEVICE];
    uint8_t              configurationCount;
};

#ifndef TEMP_BUFFER_SIZE 
#define TEMP_BUFFER_SIZE 256 
#endif

class DeviceEnumerator : public USBDriver {
private:
    enum EnumerationState { STATE_IDLE, STATE_PROCESSING_STATIC, STATE_DONE, STATE_ERROR };

public:
    DeviceEnumerator(USBHost& host);

    virtual bool claim(Device_t* device, int type, const uint8_t* descriptors, uint32_t len) override;
    virtual void disconnect() override;
    virtual void control(const Transfer_t* transfer) override; 
    virtual void Task() override; 

    bool isEnumerationDone() const { return enum_state_ == STATE_DONE; }
    bool isErrorState() const { return enum_state_ == STATE_ERROR; } 
    const UsbDeviceData* getStoredDeviceData() const { return (enum_state_ == STATE_DONE) ? &stored_data_ : nullptr; }
    Device_t* getCurrentDevice() const { return current_device_; } 

    void printStoredData(Print& printer) const; 
    void clearStoredDeviceData(); // Public

    static const uint8_t dummy_kbd_report_desc[];
    static const uint16_t dummy_kbd_report_desc_len;
    static const uint8_t dummy_mouse_report_desc[];
    static const uint16_t dummy_mouse_report_desc_len;

// Changed to protected as these are internal helpers for the dummy version's operation
// or for the real one later. The public interface is through claim/Task/getStoredDeviceData.
protected: 
    void init();
    void populateStaticDataWithCompositeDevice(); // Specific to dummy

    // These are the methods that the "no declaration matches" errors referred to.
    // They are part of the full enumerator's design, but NOPs in the dummy.
    // They should be declared (even if NOP) to match the .cpp file.
    void processStateMachine(const Transfer_t* transfer = nullptr); 
    void queueGetDescriptor(uint8_t desc_type, uint8_t desc_index, uint16_t lang_id, uint16_t len);
    void queueGetHidReportDescriptor(uint8_t config_idx, uint8_t interface_idx, uint8_t report_desc_info_idx);
    void storeString(char* out_buffer, size_t out_buffer_len, const Transfer_t* transfer_with_string_data);
    void parseFullConfigurationBlock(); 

    // Internal print helpers
    void _print_endpoint_attributes_debug(uint8_t attr) const;
    void _print_endpoint_interval_debug(uint8_t interval, uint8_t speed, uint8_t attributes) const;


private:
    Device_t*         current_device_;
    EnumerationState  enum_state_;
    UsbDeviceData     stored_data_;
    bool              data_populated_this_claim_; 
};

#endif // DEVICE_ENUMERATOR_H