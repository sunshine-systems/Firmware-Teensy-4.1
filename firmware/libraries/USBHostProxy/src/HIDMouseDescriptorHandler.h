#ifndef _HIDMOUSEDESCRIPTORHANDLER_H_
#define _HIDMOUSEDESCRIPTORHANDLER_H_

#include <Arduino.h>
#include "USBHostDriver.h"

// HID item types
#define HID_ITEM_TYPE_MAIN      0x00
#define HID_ITEM_TYPE_GLOBAL    0x04
#define HID_ITEM_TYPE_LOCAL     0x08

// HID main items
#define HID_MAIN_INPUT          0x80
#define HID_MAIN_OUTPUT         0x90
#define HID_MAIN_COLLECTION     0xA0
#define HID_MAIN_FEATURE        0xB0
#define HID_MAIN_END_COLLECTION 0xC0

// HID global items
#define HID_GLOBAL_USAGE_PAGE   0x04
#define HID_GLOBAL_LOGICAL_MIN  0x14
#define HID_GLOBAL_LOGICAL_MAX  0x24
#define HID_GLOBAL_PHYSICAL_MIN 0x34
#define HID_GLOBAL_PHYSICAL_MAX 0x44
#define HID_GLOBAL_UNIT_EXP     0x54
#define HID_GLOBAL_UNIT         0x64
#define HID_GLOBAL_REPORT_SIZE  0x74
#define HID_GLOBAL_REPORT_ID    0x84
#define HID_GLOBAL_REPORT_COUNT 0x94
#define HID_GLOBAL_PUSH         0xA4
#define HID_GLOBAL_POP          0xB4

// HID local items
#define HID_LOCAL_USAGE         0x08
#define HID_LOCAL_USAGE_MIN     0x18
#define HID_LOCAL_USAGE_MAX     0x28

// HID usage pages
#define HID_USAGE_PAGE_GENERIC_DESKTOP  0x01
#define HID_USAGE_PAGE_BUTTON           0x09

// HID generic desktop usages
#define HID_USAGE_POINTER       0x01
#define HID_USAGE_MOUSE         0x02
#define HID_USAGE_X             0x30
#define HID_USAGE_Y             0x31
#define HID_USAGE_Z             0x32
#define HID_USAGE_WHEEL         0x38

// Field information structure
struct FieldInfo {
    uint8_t usage_page;
    uint8_t usage;
    uint16_t bit_offset;
    uint8_t bit_count;
    int32_t logical_min;
    int32_t logical_max;
    bool is_relative;
    bool is_signed;
};

// Mouse state structure
struct MouseState {
    int16_t x;
    int16_t y;
    int8_t wheel;
    uint8_t buttons;
    
    void clear() {
        x = 0;
        y = 0;
        wheel = 0;
        buttons = 0;
    }
    
    bool leftButton() const { return buttons & 0x01; }
    bool rightButton() const { return buttons & 0x02; }
    bool middleButton() const { return buttons & 0x04; }
    bool button4() const { return buttons & 0x08; }  // Thumb back/side button 1
    bool button5() const { return buttons & 0x10; }  // Thumb forward/side button 2
    bool button6() const { return buttons & 0x20; }
    bool button7() const { return buttons & 0x40; }
    bool button8() const { return buttons & 0x80; }
};

// Parse state for descriptor parsing
struct ParseState {
    uint8_t usage_page;
    uint8_t usage;
    int32_t logical_min;
    int32_t logical_max;
    uint8_t report_size;
    uint8_t report_count;
    uint8_t report_id;
};

// Handler states
enum HIDHandlerState {
    HID_STATE_IDLE,
    HID_STATE_WAIT_DESCRIPTOR,
    HID_STATE_PARSING,
    HID_STATE_READY,
    HID_STATE_ERROR
};

class HIDMouseDescriptorHandler {
public:
    HIDMouseDescriptorHandler();
    
    // Initialize with a USBHostDriver
    void begin(USBHostDriver* driver);
    
    // Find and setup mouse interface
    bool setupMouseInterface();
    
    // Request and parse HID descriptor from USB device
    bool requestHIDDescriptor(uint32_t timeout_ms = 500);
    
    // Activate the HID interface (send SET_IDLE and SET_PROTOCOL)
    bool activateInterface();
    
    // Set to standard boot mouse format (fallback)
    void setBootMouseFormat();
    
    // Convert between raw HID data and MouseState
    bool parseMouseData(const uint8_t* raw_data, uint32_t length, MouseState& state);
    bool formatMouseData(const MouseState& state, uint8_t* raw_data, uint32_t& length);
    
    // State and status
    HIDHandlerState getState() const { return handler_state; }
    bool isReady() const { return handler_state == HID_STATE_READY && valid; }
    bool isValid() const { return valid; }
    uint8_t getReportID() const { return report_id; }
    uint8_t getReportSize() const { return report_size_bytes; }
    uint8_t getReportSizeBytes() const { return report_size_bytes; }
    bool hasX() const { return has_x; }
    bool hasY() const { return has_y; }
    bool hasWheel() const { return has_wheel; }
    bool hasButtons() const { return has_buttons; }
    uint8_t getButtonCount() const { return button_count; }
    
    // Interface information
    uint8_t getInterfaceIndex() const { return interface_index; }
    uint8_t getInterfaceNumber() const { return interface_number; }
    uint8_t getEndpointAddress() const { return endpoint_address; }
    uint16_t getEndpointSize() const { return endpoint_size; }
    
    // Check if device uses Report ID
    bool hasReportId() const { return report_id > 0; }
    
    // Get button byte offset (which byte contains button data)
    uint8_t getButtonByteOffset() const { 
        if (!has_buttons) return 0;
        return buttons_field.bit_offset / 8; 
    }
    
    // Debug output
    void printInterfaceInfo();
    void printDescriptorInfo();
    
    // Enable/disable debug output
    void setDebugOutput(bool enable) { debug_enabled = enable; }
    bool getDebugOutput() const { return debug_enabled; }
    
private:
    // USB Host Driver reference
    USBHostDriver* host_driver;
    
    // Handler state
    HIDHandlerState handler_state;
    
    // Interface information
    uint8_t interface_index;     // Index in driver's interface array
    uint8_t interface_number;    // USB interface number
    uint8_t interface_protocol;  // 1=keyboard, 2=mouse
    uint16_t descriptor_length;  // Expected HID descriptor length
    
    // Endpoint information
    uint8_t endpoint_address;
    uint16_t endpoint_size;
    uint8_t endpoint_interval;
    
    // HID descriptor storage
    uint8_t hid_descriptor[512];
    uint16_t hid_descriptor_size;
    
    // Helper methods for USB communication
    bool findMouseInterface();
    bool retrieveHIDDescriptor(uint32_t timeout_ms);
    
    // HID Report Parser functionality (from original HIDReportParser)
    bool parseDescriptor(const uint8_t* descriptor, uint16_t length);
    uint8_t getItemSize(uint8_t byte0);
    int32_t extractValue(const uint8_t* data, uint16_t bit_offset, uint8_t bit_count,
                        int32_t logical_min, int32_t logical_max, bool is_signed);
    void insertValue(uint8_t* data, uint16_t bit_offset, uint8_t bit_count,
                    int32_t value, int32_t logical_min, int32_t logical_max);
    
    // Parser state
    bool valid;
    uint8_t report_id;
    uint8_t report_size_bytes;
    uint16_t total_bits;
    
    // Field presence flags
    bool has_x;
    bool has_y;
    bool has_wheel;
    bool has_buttons;
    uint8_t button_count;
    
    // Field information
    FieldInfo x_field;
    FieldInfo y_field;
    FieldInfo wheel_field;
    FieldInfo buttons_field;
    
    // Debug output control
    bool debug_enabled;
};

#endif // _HIDMOUSEDESCRIPTORHANDLER_H_