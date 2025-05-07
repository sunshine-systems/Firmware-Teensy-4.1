#ifndef DEVICE_ENUMERATOR_H
#define DEVICE_ENUMERATOR_H

// --- Standard Includes ---
#include <Arduino.h>     // For Print class (used by printStoredData)
#include <USBHost_t36.h> // Core USB Host library
#include <math.h>        // For pow() if used in print helpers
#include <stddef.h>      // For offsetof()
#include <string.h>      // For memset/memcpy
#include <new>           // For nothrow new

// --- Logging Configuration for DeviceEnumerator ---
// This flag controls logging *within* DeviceEnumerator.cpp
// The actual printf output goes to the debug serial port configured by debugprintf.c
#define ENABLE_DE_LOGGING true // Set to true or false as needed for DeviceEnumerator verbosity

// --- Standard USB Definitions ---
// (Ensure these match or are a superset of what your .cpp needs)
#define USB_REQUEST_GET_STATUS        0
#define USB_REQUEST_CLEAR_FEATURE     1
#define USB_REQUEST_SET_FEATURE       3
#define USB_REQUEST_SET_ADDRESS       5
#define USB_REQUEST_GET_DESCRIPTOR    6
#define USB_REQUEST_SET_DESCRIPTOR    7
#define USB_REQUEST_GET_CONFIGURATION 8
#define USB_REQUEST_SET_CONFIGURATION 9
#define USB_REQUEST_GET_INTERFACE     10
#define USB_REQUEST_SET_INTERFACE     11
#define USB_REQUEST_SYNCH_FRAME       12
#define USB_DESCRIPTOR_DEVICE           1
#define USB_DESCRIPTOR_CONFIGURATION    2
#define USB_DESCRIPTOR_STRING           3
#define USB_DESCRIPTOR_INTERFACE        4
#define USB_DESCRIPTOR_ENDPOINT         5
#define USB_DESCRIPTOR_DEVICE_QUALIFIER 6
#define USB_DESCRIPTOR_OTHER_SPEED_CONFIGURATION 7
#define USB_DESCRIPTOR_INTERFACE_POWER  8
#define USB_DESCRIPTOR_OTG              9
#define USB_DESCRIPTOR_DEBUG            10
#define USB_DESCRIPTOR_INTERFACE_ASSOCIATION 11
#define USB_DESC_TYPE_HUB               0x29
#define USB_DESC_TYPE_SS_HUB            0x2A
#define USB_DESC_TYPE_ENDPOINT_COMPANION 0x30
#define USB_DESC_TYPE_HID               0x21
#define USB_DESC_TYPE_REPORT            0x22
#define USB_DESC_TYPE_PHYSICAL          0x23
#define CS_UNDEFINED            0x20 // Class-Specific
#define CS_INTERFACE            0x24
#define CS_ENDPOINT             0x25
#define USB_SPEED_FULL  0
#define USB_SPEED_LOW   1
#define USB_SPEED_HIGH  2

// --- Standard Descriptor Structures ---
// (Ensure these are complete as per your original file or USB spec for fields used)
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed)) usb_device_descriptor_t;

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed)) usb_configuration_descriptor_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} __attribute__((packed)) usb_interface_descriptor_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
    // uint8_t bRefresh; // For Isochronous Audio Synch Endpoint
    // uint8_t bSynchAddress; // For Isochronous Audio Synch Endpoint
} __attribute__((packed)) usb_endpoint_descriptor_t; // Note: Full Iso EP desc is 9 bytes

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bFirstInterface;
    uint8_t  bInterfaceCount;
    uint8_t  bFunctionClass;
    uint8_t  bFunctionSubClass;
    uint8_t  bFunctionProtocol;
    uint8_t  iFunction;
} __attribute__((packed)) usb_interface_assoc_descriptor_t;

typedef struct {
    uint8_t   bLength;
    uint8_t   bDescriptorType; // Should be 0x21 (HID)
    uint16_t  bcdHID;
    uint8_t   bCountryCode;
    uint8_t   bNumDescriptors; // Number of class descriptors that follow (e.g., Report, Physical)
    // Followed by one or more {bDescriptorType, wDescriptorLength} pairs
    struct OptionalHIDDescriptor { // Helper for parsing, not directly mapped
        uint8_t type;
        uint16_t length;
    } __attribute__((packed)) optional_descriptors[1]; // Placeholder for at least one
} __attribute__((packed)) usb_hid_descriptor_t;

typedef struct { // Generic descriptor header, useful for iterating
    uint8_t bLength;
    uint8_t bDescriptorType;
} __attribute__((packed)) usb_descriptor_t;


// --- USB Data Storage Structures (to hold parsed data) ---
#define MAX_CONFIGURATIONS_PER_DEVICE 1     // Typically only one configuration is selected/parsed
#define MAX_INTERFACES_PER_CONFIG 8         // Arbitrary limit, adjust if needed
#define MAX_ENDPOINTS_PER_INTERFACE 6       // Arbitrary limit
#define MAX_HID_REPORT_DESC_PER_INTERFACE 2 // Usually 1 Report desc, maybe 1 Physical
#define MAX_STRING_LENGTH 128               // For storing decoded string descriptors

struct UsbEndpointData {
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
    uint8_t  bSynchAddress; // Relevant for Isochronous Synch EPs
};

struct UsbHidReportDescInfo {
    uint8_t   bDescriptorType;   // Should be USB_DESC_TYPE_REPORT (0x22)
    uint16_t  wDescriptorLength; // Length of the actual report descriptor
    uint8_t*  rawData;           // Dynamically allocated buffer for the report descriptor
};

struct UsbInterfaceData {
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;         // String descriptor index
    char    interfaceString[MAX_STRING_LENGTH];
    
    UsbEndpointData endpoints[MAX_ENDPOINTS_PER_INTERFACE];
    uint8_t         endpointCount;
    
    bool     isHidInterface;
    uint16_t bcdHID;
    uint8_t  bCountryCode;
    uint8_t  bNumHidClassDescriptors; // From HID descriptor (bNumDescriptors field)
    UsbHidReportDescInfo reportDescriptors[MAX_HID_REPORT_DESC_PER_INTERFACE];
    uint8_t              reportDescriptorCount; // Actual number of report desc info stored
};

struct UsbConfigurationData {
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    char     configurationString[MAX_STRING_LENGTH];
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
    uint16_t wTotalLength;        // From this configuration descriptor header
    uint8_t* rawConfigData;       // Buffer for the entire configuration block
    uint16_t rawConfigLen;        // Length of data stored in rawConfigData
    
    UsbInterfaceData interfaces[MAX_INTERFACES_PER_CONFIG];
    uint8_t          interfaceCount;
};

struct UsbDeviceData {
    // From Device Descriptor
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;

    // From USBHost_t36 Device_t or queried
    uint8_t  speed; 
    uint8_t  hub_address;
    uint8_t  hub_port;
    uint8_t  address; 

    // Decoded Strings
    char manufacturerString[MAX_STRING_LENGTH];
    char productString[MAX_STRING_LENGTH];
    char serialNumberString[MAX_STRING_LENGTH];

    UsbConfigurationData configurations[MAX_CONFIGURATIONS_PER_DEVICE];
    uint8_t              configurationCount; 
};

// Define TEMP_BUFFER_SIZE if it's used internally by DeviceEnumerator
// This is the buffer used for individual USB control transfers.
#ifndef TEMP_BUFFER_SIZE
#define TEMP_BUFFER_SIZE 1024 // Should be >= max descriptor size expected in one transfer (e.g., full config block up to a point)
#endif


// --- Class Definition ---
class DeviceEnumerator : public USBDriver {
private:
    // --- Enumeration State Enum --- (Ensure all states your FSM uses are here)
    enum EnumerationState {
        STATE_IDLE, STATE_GETTING_DEVICE_DESC_8, STATE_GETTING_DEVICE_DESC_FULL,
        STATE_GETTING_STRING_MAN, STATE_GETTING_STRING_PROD, STATE_GETTING_STRING_SERIAL,
        STATE_GETTING_CONFIG_HEADER, STATE_GETTING_CONFIG_FULL,
        STATE_PARSING_CONFIG,
        STATE_GETTING_CONFIG_STRING,
        STATE_GETTING_INTERFACE_STRINGS,
        STATE_GETTING_REPORT_DESC,
        STATE_DONE, STATE_ERROR
    };

public:
    DeviceEnumerator(USBHost& host); // Constructor takes USBHost reference

    // --- USBDriver Virtual Methods ---
    virtual bool claim(Device_t* device, int type, const uint8_t* descriptors, uint32_t len) override;
    virtual void disconnect() override;
    virtual void control(const Transfer_t* transfer) override; 
    virtual void Task() override; 

    // --- Public Accessor Methods ---
    bool isEnumerationDone() const { return enum_state_ == STATE_DONE; }
    bool isErrorState() const { return enum_state_ == STATE_ERROR; }
    const UsbDeviceData* getStoredDeviceData() const { return (enum_state_ == STATE_DONE || enum_state_ == STATE_ERROR) ? &stored_data_ : nullptr; }
    Device_t* getCurrentDevice() const { return current_device_; }

    // --- Public Action Methods ---
    void printStoredData(Print& printer) const; 
    void clearStoredDeviceData(); // Public for startup_usbhost.cpp

protected:
    void init(); 
    void startEnumeration(Device_t* device); 
    void processStateMachine(const Transfer_t* transfer = nullptr); 
    
    void queueGetDescriptor(uint8_t desc_type, uint8_t desc_index, uint16_t lang_id, uint16_t len);
    void queueGetHidReportDescriptor(uint8_t config_idx, uint8_t interface_idx, uint8_t report_desc_info_idx);
    
    void storeString(char* out_buffer, size_t out_buffer_len, const Transfer_t* transfer_with_string_data);
    void parseFullConfigurationBlock(); 
    
    // Internal print helpers, now using global printf if logging is enabled
    void _print_endpoint_attributes_debug(uint8_t attr) const;
    void _print_endpoint_interval_debug(uint8_t interval, uint8_t speed, uint8_t attributes) const;

private:
    // --- Member Variables ---
    Device_t*         current_device_;
    EnumerationState  enum_state_;
    UsbDeviceData     stored_data_; 
    uint16_t          config_total_len_; // Expected total length of current config descriptor
    
    // Indices used during parsing/fetching sequences for the *current* configuration
    uint8_t           current_config_index_parsing_; // Index into stored_data_.configurations
    uint8_t           current_interface_index_parsing_; // Index into current config's interfaces array
    uint8_t           current_hid_report_index_parsing_; // Index into current interface's reportDescriptors array

    alignas(4) uint8_t temp_buffer_[TEMP_BUFFER_SIZE]; 
    alignas(4) setup_t setup_packet_; 
};

// Global helper (can be outside class or static inside if preferred)
// Renamed to avoid potential conflicts if other files define a similar function.
const char* descriptor_type_to_string_de(uint8_t type); 

#endif // DEVICE_ENUMERATOR_H