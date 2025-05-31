// HIDReportParser.h
#ifndef _HIDREPORTPARSER_H_
#define _HIDREPORTPARSER_H_

#include <Arduino.h>

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

class HIDReportParser {
public:
    HIDReportParser();
    
    // Parse HID report descriptor
    bool parseDescriptor(const uint8_t* descriptor, uint16_t length);
    
    // Set to standard boot mouse format (fallback)
    void setBootMouseFormat();
    
    // Convert between raw HID data and MouseState
    bool parseMouseData(const uint8_t* raw_data, uint32_t length, MouseState& state);
    bool formatMouseData(const MouseState& state, uint8_t* raw_data, uint32_t& length);
    
    // Get parsed information
    bool isValid() const { return valid; }
    uint8_t getReportID() const { return report_id; }
    uint8_t getReportSizeBytes() const { return report_size_bytes; }
    bool hasX() const { return has_x; }
    bool hasY() const { return has_y; }
    bool hasWheel() const { return has_wheel; }
    bool hasButtons() const { return has_buttons; }
    uint8_t getButtonCount() const { return button_count; }
    
    // Debug output
    void printDescriptorInfo();
    void printMouseState(const MouseState& state);
    
    // Enable/disable debug output
    void setDebugOutput(bool enable) { debug_enabled = enable; }
    bool getDebugOutput() const { return debug_enabled; }
    
private:
    // Helper functions
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

#endif // _HIDREPORTPARSER_H_