/**
 * @file HIDReportParser.cpp
 * @brief Proper HID Report Descriptor Parser Implementation
 */

#include "HIDReportParser.h"

HIDReportParser::HIDReportParser() {
    valid = false;
    report_id = 0;
    report_size_bytes = 0;
    total_bits = 0;
    has_x = false;
    has_y = false;
    has_wheel = false;
    has_buttons = false;
    button_count = 0;
    
    memset(&x_field, 0, sizeof(x_field));
    memset(&y_field, 0, sizeof(y_field));
    memset(&wheel_field, 0, sizeof(wheel_field));
    memset(&buttons_field, 0, sizeof(buttons_field));
}

uint8_t HIDReportParser::getItemSize(uint8_t byte0) {
    uint8_t size = byte0 & 0x03;
    if (size == 3) size = 4;  // Size 3 means 4 bytes
    return size;
}

bool HIDReportParser::parseDescriptor(const uint8_t* descriptor, uint16_t length) {
    Serial4.println("\n=== Parsing HID Report Descriptor ===");
    
    ParseState state = {0};
    uint16_t offset = 0;
    uint16_t current_bit = 0;
    
    // Reset fields
    has_x = false;
    has_y = false;
    has_wheel = false;
    has_buttons = false;
    button_count = 0;
    total_bits = 0;
    
    // Track usages for next INPUT
    uint8_t pending_usages[16];
    uint8_t pending_usage_count = 0;
    
    // Parse descriptor
    while (offset < length) {
        if (offset >= length) break;
        
        uint8_t byte0 = descriptor[offset];
        uint8_t size = getItemSize(byte0);
        uint8_t type = byte0 & 0xFC;
        
        if (offset + size + 1 > length) break;
        
        // Extract data value
        int32_t value = 0;
        for (uint8_t i = 0; i < size; i++) {
            value |= (uint32_t)descriptor[offset + 1 + i] << (i * 8);
        }
        
        // Handle sign extension
        if (size > 0 && (value & (1 << ((size * 8) - 1)))) {
            for (uint8_t i = size; i < 4; i++) {
                value |= (0xFF << (i * 8));
            }
        }
        
        // Debug each item
        if ((type == HID_GLOBAL_USAGE_PAGE) || 
            (type == HID_LOCAL_USAGE) || 
            (type == HID_MAIN_INPUT)) {
            Serial4.print("  [");
            Serial4.print(offset, HEX);
            Serial4.print("] ");
            
            switch(type) {
                case HID_GLOBAL_USAGE_PAGE:
                    Serial4.print("Usage Page: 0x");
                    Serial4.println(value, HEX);
                    break;
                case HID_LOCAL_USAGE:
                    Serial4.print("Usage: 0x");
                    Serial4.print(value, HEX);
                    if (state.usage_page == HID_USAGE_PAGE_GENERIC_DESKTOP) {
                        Serial4.print(" (");
                        switch(value) {
                            case HID_USAGE_X: Serial4.print("X"); break;
                            case HID_USAGE_Y: Serial4.print("Y"); break;
                            case HID_USAGE_WHEEL: Serial4.print("Wheel"); break;
                            case HID_USAGE_POINTER: Serial4.print("Pointer"); break;
                            case HID_USAGE_MOUSE: Serial4.print("Mouse"); break;
                            default: Serial4.print("?"); break;
                        }
                        Serial4.print(")");
                    }
                    Serial4.println();
                    break;
                case HID_MAIN_INPUT:
                    Serial4.print("Input: Count=");
                    Serial4.print(state.report_count);
                    Serial4.print(" Size=");
                    Serial4.print(state.report_size);
                    Serial4.print(" Bits @ offset ");
                    Serial4.println(current_bit);
                    break;
            }
        }
        
        // Process based on item type
        switch (type) {
            case HID_GLOBAL_USAGE_PAGE:
                state.usage_page = value & 0xFF;
                pending_usage_count = 0; // Clear pending usages on new page
                break;
                
            case HID_LOCAL_USAGE:
                // Store usage for next INPUT
                if (pending_usage_count < 16) {
                    pending_usages[pending_usage_count++] = value & 0xFF;
                }
                break;
                
            case HID_LOCAL_USAGE_MIN:
                // Button usage min
                if (state.usage_page == HID_USAGE_PAGE_BUTTON) {
                    if (pending_usage_count < 16) {
                        pending_usages[pending_usage_count++] = 1; // Generic button
                    }
                }
                break;
                
            case HID_GLOBAL_LOGICAL_MIN:
                state.logical_min = value;
                break;
                
            case HID_GLOBAL_LOGICAL_MAX:
                state.logical_max = value;
                break;
                
            case HID_GLOBAL_REPORT_SIZE:
                state.report_size = value & 0xFF;
                break;
                
            case HID_GLOBAL_REPORT_COUNT:
                state.report_count = value & 0xFF;
                break;
                
            case HID_GLOBAL_REPORT_ID:
                state.report_id = value & 0xFF;
                current_bit = 8;  // Report ID takes first byte
                break;
                
            case HID_MAIN_INPUT:
                // Process all pending usages with this input
                for (uint8_t i = 0; i < pending_usage_count && i < state.report_count; i++) {
                    uint8_t usage = pending_usages[i];
                    uint16_t field_bit_offset = current_bit + (i * state.report_size);
                    
                    if (state.usage_page == HID_USAGE_PAGE_GENERIC_DESKTOP) {
                        if (usage == HID_USAGE_X && !has_x) {
                            x_field.usage_page = state.usage_page;
                            x_field.usage = usage;
                            x_field.bit_offset = field_bit_offset;
                            x_field.bit_count = state.report_size;
                            x_field.logical_min = state.logical_min;
                            x_field.logical_max = state.logical_max;
                            x_field.is_relative = (value & 0x04) != 0;
                            x_field.is_signed = state.logical_min < 0;
                            has_x = true;
                            
                            Serial4.print(">>> Found X: offset=");
                            Serial4.print(x_field.bit_offset);
                            Serial4.print(" bits=");
                            Serial4.print(x_field.bit_count);
                            Serial4.print(" range=");
                            Serial4.print(x_field.logical_min);
                            Serial4.print("..");
                            Serial4.println(x_field.logical_max);
                        }
                        else if (usage == HID_USAGE_Y && !has_y) {
                            y_field.usage_page = state.usage_page;
                            y_field.usage = usage;
                            y_field.bit_offset = field_bit_offset;
                            y_field.bit_count = state.report_size;
                            y_field.logical_min = state.logical_min;
                            y_field.logical_max = state.logical_max;
                            y_field.is_relative = (value & 0x04) != 0;
                            y_field.is_signed = state.logical_min < 0;
                            has_y = true;
                            
                            Serial4.print(">>> Found Y: offset=");
                            Serial4.print(y_field.bit_offset);
                            Serial4.print(" bits=");
                            Serial4.print(y_field.bit_count);
                            Serial4.print(" range=");
                            Serial4.print(y_field.logical_min);
                            Serial4.print("..");
                            Serial4.println(y_field.logical_max);
                        }
                        else if (usage == HID_USAGE_WHEEL && !has_wheel) {
                            wheel_field.usage_page = state.usage_page;
                            wheel_field.usage = usage;
                            wheel_field.bit_offset = field_bit_offset;
                            wheel_field.bit_count = state.report_size;
                            wheel_field.logical_min = state.logical_min;
                            wheel_field.logical_max = state.logical_max;
                            wheel_field.is_relative = (value & 0x04) != 0;
                            wheel_field.is_signed = state.logical_min < 0;
                            has_wheel = true;
                            
                            Serial4.print(">>> Found Wheel: offset=");
                            Serial4.print(wheel_field.bit_offset);
                            Serial4.print(" bits=");
                            Serial4.println(wheel_field.bit_count);
                        }
                    }
                    else if (state.usage_page == HID_USAGE_PAGE_BUTTON && !has_buttons) {
                        buttons_field.usage_page = state.usage_page;
                        buttons_field.usage = usage;
                        buttons_field.bit_offset = current_bit;
                        buttons_field.bit_count = state.report_size * state.report_count;
                        buttons_field.logical_min = state.logical_min;
                        buttons_field.logical_max = state.logical_max;
                        button_count = state.report_count;
                        has_buttons = true;
                        
                        Serial4.print(">>> Found Buttons: offset=");
                        Serial4.print(buttons_field.bit_offset);
                        Serial4.print(" count=");
                        Serial4.print(button_count);
                        Serial4.print(" bits=");
                        Serial4.println(buttons_field.bit_count);
                    }
                }
                
                // Clear pending usages
                pending_usage_count = 0;
                
                // Advance bit position
                current_bit += state.report_size * state.report_count;
                break;
        }
        
        offset += size + 1;
    }
    
    // Calculate report size
    total_bits = current_bit;
    report_size_bytes = (total_bits + 7) / 8;
    report_id = state.report_id;
    
    Serial4.print("\n[PARSER] Total bits: ");
    Serial4.print(total_bits);
    Serial4.print(" = ");
    Serial4.print(report_size_bytes);
    Serial4.println(" bytes");
    
    Serial4.print("[PARSER] Found: ");
    if (has_buttons) Serial4.print("Buttons ");
    if (has_x) Serial4.print("X ");
    if (has_y) Serial4.print("Y ");
    if (has_wheel) Serial4.print("Wheel");
    Serial4.println();
    
    // Check if we have minimum requirements
    if (has_x && has_y) {
        valid = true;
        return true;
    }
    
    // If parsing failed or incomplete, use boot mouse format
    Serial4.println("\n[PARSER] Missing X or Y - using boot mouse format");
    setBootMouseFormat();
    return true;  // Return true because we have a valid format now
}

void HIDReportParser::setBootMouseFormat() {
    // Standard USB boot mouse format
    valid = true;
    report_size_bytes = 3; // Can be 3-8 bytes
    report_id = 0;   // No report ID in boot protocol
    
    // Reset all fields
    has_x = true;
    has_y = true;
    has_wheel = true;
    has_buttons = true;
    
    // Buttons: first byte, bits 0-2
    button_count = 3;
    buttons_field.bit_offset = 0;
    buttons_field.bit_count = 8;  // Full byte for compatibility
    buttons_field.logical_min = 0;
    buttons_field.logical_max = 255;
    
    // X axis: second byte, signed 8-bit
    x_field.bit_offset = 8;
    x_field.bit_count = 8;
    x_field.logical_min = -127;
    x_field.logical_max = 127;
    x_field.is_signed = true;
    x_field.is_relative = true;
    
    // Y axis: third byte, signed 8-bit
    y_field.bit_offset = 16;
    y_field.bit_count = 8;
    y_field.logical_min = -127;
    y_field.logical_max = 127;
    y_field.is_signed = true;
    y_field.is_relative = true;
    
    // Wheel: fourth byte (if present), signed 8-bit
    wheel_field.bit_offset = 24;
    wheel_field.bit_count = 8;
    wheel_field.logical_min = -127;
    wheel_field.logical_max = 127;
    wheel_field.is_signed = true;
    wheel_field.is_relative = true;
    
    Serial4.println("[PARSER] Set boot mouse format");
}

int32_t HIDReportParser::extractValue(const uint8_t* data, uint16_t bit_offset, uint8_t bit_count,
                                     int32_t logical_min, int32_t logical_max, bool is_signed) {
    // Special handling for byte-aligned multi-byte values (common case)
    if ((bit_offset % 8) == 0 && (bit_count % 8) == 0) {
        uint8_t byte_offset = bit_offset / 8;
        uint8_t byte_count = bit_count / 8;
        uint32_t value = 0;
        
        // Extract value in little-endian format
        for (uint8_t i = 0; i < byte_count && i < 4; i++) {
            value |= (uint32_t)data[byte_offset + i] << (i * 8);
        }
        
        // Handle sign extension for signed values
        if (is_signed && byte_count < 4) {
            uint32_t sign_bit = 1UL << ((byte_count * 8) - 1);
            if (value & sign_bit) {
                // Extend sign bits
                for (uint8_t i = byte_count; i < 4; i++) {
                    value |= (0xFFUL << (i * 8));
                }
            }
        }
        
        return (int32_t)value;
    }
    
    // General bit-level extraction (for non-byte-aligned fields)
    uint32_t value = 0;
    
    // Extract bits
    for (uint8_t i = 0; i < bit_count && i < 32; i++) {
        uint16_t bit_pos = bit_offset + i;
        uint8_t byte_pos = bit_pos / 8;
        uint8_t bit_in_byte = bit_pos % 8;
        
        if (data[byte_pos] & (1 << bit_in_byte)) {
            value |= (1UL << i);
        }
    }
    
    // Handle sign extension
    if (is_signed && bit_count < 32 && (value & (1UL << (bit_count - 1)))) {
        for (uint8_t i = bit_count; i < 32; i++) {
            value |= (1UL << i);
        }
    }
    
    return (int32_t)value;
}

void HIDReportParser::insertValue(uint8_t* data, uint16_t bit_offset, uint8_t bit_count,
                                 int32_t value, int32_t logical_min, int32_t logical_max) {
    // Clamp to logical range
    if (value < logical_min) value = logical_min;
    if (value > logical_max) value = logical_max;
    
    // Special handling for byte-aligned multi-byte values (common case)
    if ((bit_offset % 8) == 0 && (bit_count % 8) == 0) {
        uint8_t byte_offset = bit_offset / 8;
        uint8_t byte_count = bit_count / 8;
        
        // Clear target bytes
        for (uint8_t i = 0; i < byte_count; i++) {
            data[byte_offset + i] = 0;
        }
        
        // Insert value in little-endian format
        uint32_t uvalue = (uint32_t)value;
        for (uint8_t i = 0; i < byte_count && i < 4; i++) {
            data[byte_offset + i] = (uvalue >> (i * 8)) & 0xFF;
        }
        return;
    }
    
    // General bit-level insertion (for non-byte-aligned fields)
    uint32_t uvalue = (uint32_t)value;
    
    // Clear target bits first
    for (uint8_t i = 0; i < bit_count && i < 32; i++) {
        uint16_t bit_pos = bit_offset + i;
        uint8_t byte_pos = bit_pos / 8;
        uint8_t bit_in_byte = bit_pos % 8;
        
        data[byte_pos] &= ~(1 << bit_in_byte);
    }
    
    // Insert value bits
    for (uint8_t i = 0; i < bit_count && i < 32; i++) {
        uint16_t bit_pos = bit_offset + i;
        uint8_t byte_pos = bit_pos / 8;
        uint8_t bit_in_byte = bit_pos % 8;
        
        if (uvalue & (1UL << i)) {
            data[byte_pos] |= (1 << bit_in_byte);
        }
    }
}

bool HIDReportParser::parseMouseData(const uint8_t* raw_data, uint32_t length, MouseState& state) {
    if (!valid) {
        return false;
    }
    
    // Clear state
    state.clear();
    
    // Skip report ID if present
    const uint8_t* data = raw_data;
    if (report_id > 0 && length > 0) {
        if (raw_data[0] == report_id) {
            data++;
            length--;
        }
    }
    
    // Minimum size check
    uint32_t min_required_size = report_size_bytes;
    if (report_id > 0) min_required_size--;
    if (length < min_required_size) {
        return false;
    }
    
    // Extract buttons
    if (has_buttons) {
        uint32_t button_bits = extractValue(data, buttons_field.bit_offset, 
                                          buttons_field.bit_count,
                                          buttons_field.logical_min, 
                                          buttons_field.logical_max, false);
        state.buttons = button_bits & 0xFF;  // Take up to 8 buttons
    }
    
    // Extract X
    if (has_x) {
        state.x = extractValue(data, x_field.bit_offset, x_field.bit_count,
                              x_field.logical_min, x_field.logical_max, 
                              x_field.is_signed);
    }
    
    // Extract Y
    if (has_y) {
        state.y = extractValue(data, y_field.bit_offset, y_field.bit_count,
                              y_field.logical_min, y_field.logical_max,
                              y_field.is_signed);
    }
    
    // Extract wheel
    if (has_wheel) {
        state.wheel = extractValue(data, wheel_field.bit_offset, wheel_field.bit_count,
                                  wheel_field.logical_min, wheel_field.logical_max,
                                  wheel_field.is_signed);
    }
    
    return true;
}

bool HIDReportParser::formatMouseData(const MouseState& state, uint8_t* raw_data, uint32_t& length) {
    if (!valid) {
        return false;
    }
    
    // Set output length
    length = report_size_bytes;
    
    // Clear buffer
    memset(raw_data, 0, length);
    
    // Add report ID if needed
    uint8_t* data = raw_data;
    if (report_id > 0) {
        raw_data[0] = report_id;
        data = raw_data + 1;
    }
    
    // Insert buttons
    if (has_buttons) {
        insertValue(data, buttons_field.bit_offset, buttons_field.bit_count,
                   state.buttons, buttons_field.logical_min, buttons_field.logical_max);
    }
    
    // Insert X
    if (has_x) {
        insertValue(data, x_field.bit_offset, x_field.bit_count,
                   state.x, x_field.logical_min, x_field.logical_max);
    }
    
    // Insert Y
    if (has_y) {
        insertValue(data, y_field.bit_offset, y_field.bit_count,
                   state.y, y_field.logical_min, y_field.logical_max);
    }
    
    // Insert wheel
    if (has_wheel) {
        insertValue(data, wheel_field.bit_offset, wheel_field.bit_count,
                   state.wheel, wheel_field.logical_min, wheel_field.logical_max);
    }
    
    return true;
}

void HIDReportParser::printDescriptorInfo() {
    Serial4.println("\n=== HID Report Structure ===");
    Serial4.print("Valid: ");
    Serial4.println(valid ? "Yes" : "No");
    
    if (!valid) return;
    
    Serial4.print("Report ID: ");
    Serial4.println(report_id);
    Serial4.print("Report Size: ");
    Serial4.print(report_size_bytes);
    Serial4.print(" bytes (");
    Serial4.print(total_bits);
    Serial4.println(" bits)");
    
    if (has_buttons) {
        Serial4.print("\nButtons: ");
        Serial4.print(button_count);
        Serial4.println(" buttons");
        Serial4.print("  Bit offset: ");
        Serial4.println(buttons_field.bit_offset);
        Serial4.print("  Total bits: ");
        Serial4.println(buttons_field.bit_count);
    }
    
    if (has_x) {
        Serial4.println("\nX Axis:");
        Serial4.print("  Bit offset: ");
        Serial4.print(x_field.bit_offset);
        Serial4.print(" (byte ");
        Serial4.print(x_field.bit_offset / 8);
        Serial4.print(" bit ");
        Serial4.print(x_field.bit_offset % 8);
        Serial4.println(")");
        Serial4.print("  Size: ");
        Serial4.print(x_field.bit_count);
        Serial4.println(" bits");
        Serial4.print("  Range: ");
        Serial4.print(x_field.logical_min);
        Serial4.print(" to ");
        Serial4.println(x_field.logical_max);
        Serial4.print("  Type: ");
        Serial4.print(x_field.is_relative ? "Relative" : "Absolute");
        Serial4.print(", ");
        Serial4.println(x_field.is_signed ? "Signed" : "Unsigned");
    }
    
    if (has_y) {
        Serial4.println("\nY Axis:");
        Serial4.print("  Bit offset: ");
        Serial4.print(y_field.bit_offset);
        Serial4.print(" (byte ");
        Serial4.print(y_field.bit_offset / 8);
        Serial4.print(" bit ");
        Serial4.print(y_field.bit_offset % 8);
        Serial4.println(")");
        Serial4.print("  Size: ");
        Serial4.print(y_field.bit_count);
        Serial4.println(" bits");
        Serial4.print("  Range: ");
        Serial4.print(y_field.logical_min);
        Serial4.print(" to ");
        Serial4.println(y_field.logical_max);
        Serial4.print("  Type: ");
        Serial4.print(y_field.is_relative ? "Relative" : "Absolute");
        Serial4.print(", ");
        Serial4.println(y_field.is_signed ? "Signed" : "Unsigned");
    }
    
    if (has_wheel) {
        Serial4.println("\nWheel:");
        Serial4.print("  Bit offset: ");
        Serial4.print(wheel_field.bit_offset);
        Serial4.print(" (byte ");
        Serial4.print(wheel_field.bit_offset / 8);
        Serial4.print(" bit ");
        Serial4.print(wheel_field.bit_offset % 8);
        Serial4.println(")");
        Serial4.print("  Size: ");
        Serial4.print(wheel_field.bit_count);
        Serial4.println(" bits");
    }
    
    Serial4.println("==========================\n");
}

void HIDReportParser::printMouseState(const MouseState& state) {
    Serial4.print("Mouse: Buttons=0x");
    Serial4.print(state.buttons, HEX);
    Serial4.print(" (");
    if (state.leftButton()) Serial4.print("L");
    if (state.rightButton()) Serial4.print("R"); 
    if (state.middleButton()) Serial4.print("M");
    Serial4.print(") X=");
    Serial4.print(state.x);
    Serial4.print(" Y=");
    Serial4.print(state.y);
    Serial4.print(" Wheel=");
    Serial4.println(state.wheel);
}