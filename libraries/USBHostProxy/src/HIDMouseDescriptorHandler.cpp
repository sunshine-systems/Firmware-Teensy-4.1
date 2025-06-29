#include "HIDMouseDescriptorHandler.h"
#include "SunBoxStartup.h"
#include "SunBoxLogger.h"

HIDMouseDescriptorHandler::HIDMouseDescriptorHandler() 
    : host_driver(nullptr), handler_state(HID_STATE_IDLE),
      interface_index(0xFF), interface_number(0xFF), interface_protocol(0),
      descriptor_length(0), endpoint_address(0), endpoint_size(0),
      endpoint_interval(0), hid_descriptor_size(0),
      valid(false), report_id(0), report_size_bytes(0), total_bits(0),
      has_x(false), has_y(false), has_wheel(false), has_buttons(false),
      button_count(0), debug_enabled(false) {
    
    memset(hid_descriptor, 0, sizeof(hid_descriptor));
    memset(&x_field, 0, sizeof(x_field));
    memset(&y_field, 0, sizeof(y_field));
    memset(&wheel_field, 0, sizeof(wheel_field));
    memset(&buttons_field, 0, sizeof(buttons_field));
}

void HIDMouseDescriptorHandler::begin(USBHostDriver* driver) {
    host_driver = driver;
    handler_state = HID_STATE_IDLE;
    debug_enabled = SunBoxStartup::isDebugEnabled();
    
    if (debug_enabled) {
        logger.startup("Starting HID Mouse Descriptor Parsing..");
    }
}

bool HIDMouseDescriptorHandler::setupMouseInterface() {
    if (!host_driver || !host_driver->isReady()) {
        if (debug_enabled) {
            logger.warning("No driver or device not ready");
        }
        return false;
    }
    
    // Find mouse interface
    if (!findMouseInterface()) {
        if (debug_enabled) {
            logger.warning("No mouse interface found");
        }
        handler_state = HID_STATE_ERROR;
        return false;
    }
    
    if (debug_enabled) {
        logger.debugf("Found mouse interface %d at index %d", interface_number, interface_index);
    }
    
    return true;
}

bool HIDMouseDescriptorHandler::findMouseInterface() {
    // First, look for HID mouse interface (class=3, protocol=2)
    int8_t idx = host_driver->findInterface(3, 0xFF, 2);
    if (idx >= 0) {
        interface_index = idx;
        interface_number = host_driver->getInterfaceNumber(idx);
        interface_protocol = host_driver->getInterfaceProtocol(idx);
        descriptor_length = host_driver->getHIDDescriptorLength(idx);
        endpoint_address = host_driver->getEndpointAddress(idx);
        endpoint_size = host_driver->getEndpointSize(idx);
        endpoint_interval = host_driver->getEndpointInterval(idx);
        
        if (debug_enabled) {
            logger.debug("Found HID mouse interface (protocol=2)");
        }
        return true;
    }
    
    // If not found, look for any HID interface
    idx = host_driver->findInterface(3);
    if (idx >= 0) {
        interface_index = idx;
        interface_number = host_driver->getInterfaceNumber(idx);
        interface_protocol = host_driver->getInterfaceProtocol(idx);
        descriptor_length = host_driver->getHIDDescriptorLength(idx);
        endpoint_address = host_driver->getEndpointAddress(idx);
        endpoint_size = host_driver->getEndpointSize(idx);
        endpoint_interval = host_driver->getEndpointInterval(idx);
        
        if (debug_enabled) {
            logger.debugf("Found HID interface with protocol=%d", interface_protocol);
        }
        return true;
    }
    
    return false;
}

bool HIDMouseDescriptorHandler::requestHIDDescriptor(uint32_t timeout_ms) {
    if (!host_driver || interface_index == 0xFF) {
        if (debug_enabled) {
            logger.warning("Cannot request descriptor - no interface");
        }
        return false;
    }
    
    handler_state = HID_STATE_WAIT_DESCRIPTOR;
    
    if (debug_enabled) {
        logger.debugf("Requesting HID descriptor for interface %d, expected length: %d", 
                     interface_number, descriptor_length);
    }
    
    // Try to retrieve the HID descriptor
    if (!retrieveHIDDescriptor(timeout_ms)) {
        // If retrieval failed, use boot protocol
        if (debug_enabled) {
            logger.warning("Descriptor retrieval failed, using boot protocol");
        }
        setBootMouseFormat();
        handler_state = HID_STATE_READY;
        return true;  // Boot protocol is still valid
    }
    
    // Parse the descriptor
    handler_state = HID_STATE_PARSING;
    if (!parseDescriptor(hid_descriptor, hid_descriptor_size)) {
        // If parsing failed, try boot protocol
        if (debug_enabled) {
            logger.warning("Parse failed, using boot protocol");
        }
        setBootMouseFormat();
    }
    
    handler_state = HID_STATE_READY;
    
    if (debug_enabled) {
        logger.debug("HID descriptor ready!");
    }
    
    return true;
}

bool HIDMouseDescriptorHandler::retrieveHIDDescriptor(uint32_t timeout_ms) {
    // If descriptor length is 0, we might not have a report descriptor
    if (descriptor_length == 0) {
        if (debug_enabled) {
            logger.warning("No HID descriptor length, using boot protocol");
        }
        return false;
    }
    
    uint16_t actual_length = 0;
    uint16_t request_length = min(descriptor_length, (uint16_t)512);
    
    // USB HID Report Descriptor request
    bool success = host_driver->controlTransfer(
        0x81,                    // bmRequestType
        0x06,                    // bRequest (GET_DESCRIPTOR)
        0x2200,                  // wValue (Report descriptor)
        interface_number,        // wIndex
        request_length,          // wLength
        hid_descriptor,          // data buffer
        &actual_length,          // actual length received
        timeout_ms
    );
    
    if (success && actual_length > 0) {
        hid_descriptor_size = actual_length;
        
        if (debug_enabled) {
            logger.debugf("Retrieved %d bytes of HID descriptor", actual_length);
            
            // Print first few bytes for debugging
            logger.debug("First bytes: ");
            for (uint16_t i = 0; i < min(actual_length, (uint16_t)16); i++) {
                logger.debugf("%02X ", hid_descriptor[i]);
            }
        }
        
        return true;
    }
    
    if (debug_enabled) {
        logger.warning("Failed to retrieve HID descriptor");
    }
    
    return false;
}

bool HIDMouseDescriptorHandler::activateInterface() {
    if (!host_driver || interface_index == 0xFF) {
        if (debug_enabled) {
            logger.error("Error cannot activate no device interface found, report this error to the developer");
        }
        return false;
    }
    
    if (debug_enabled) {
        logger.debug("Interface activation complete");
    }
    
    // Note: SET_IDLE and SET_PROTOCOL are optional HID commands that many devices
    // don't support. We've removed them since they often return STALL.
    // The device will work fine without them.
    
    return true;
}

// ========== HID Report Parser Logic (from original HIDReportParser) ==========

uint8_t HIDMouseDescriptorHandler::getItemSize(uint8_t byte0) {
    uint8_t size = byte0 & 0x03;
    if (size == 3) size = 4;  // Size 3 means 4 bytes
    return size;
}

bool HIDMouseDescriptorHandler::parseDescriptor(const uint8_t* descriptor, uint16_t length) {
    bool debug_enabled = SunBoxStartup::isDebugEnabled();
    
    if (debug_enabled) {
        logger.debug("\n=== Parsing HID Report Descriptor ===");
    }
    
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
        if (debug_enabled && ((type == HID_GLOBAL_USAGE_PAGE) || 
            (type == HID_LOCAL_USAGE) || 
            (type == HID_MAIN_INPUT))) {
            logger.debugf("  [%X] ", offset);
            
            switch(type) {
                case HID_GLOBAL_USAGE_PAGE:
                    logger.debugf("Usage Page: 0x%X", value);
                    break;
                case HID_LOCAL_USAGE:
                    logger.debugf("Usage: 0x%X", value);
                    if (state.usage_page == HID_USAGE_PAGE_GENERIC_DESKTOP) {
                        switch(value) {
                            case HID_USAGE_X: logger.debug(" (X)"); break;
                            case HID_USAGE_Y: logger.debug(" (Y)"); break;
                            case HID_USAGE_WHEEL: logger.debug(" (Wheel)"); break;
                            case HID_USAGE_POINTER: logger.debug(" (Pointer)"); break;
                            case HID_USAGE_MOUSE: logger.debug(" (Mouse)"); break;
                            default: logger.debug(" (?)"); break;
                        }
                    }
                    break;
                case HID_MAIN_INPUT:
                    logger.debugf("Input: Count=%d Size=%d Bits @ offset %d", 
                                 state.report_count, state.report_size, current_bit);
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
                            
                            logger.debugf(">>> Found X: offset=%d bits=%d range=%d..%d", 
                                        x_field.bit_offset, x_field.bit_count, 
                                        x_field.logical_min, x_field.logical_max);
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
                            
                            logger.debugf(">>> Found Y: offset=%d bits=%d range=%d..%d", 
                                        y_field.bit_offset, y_field.bit_count, 
                                        y_field.logical_min, y_field.logical_max);
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
                            
                            logger.debugf(">>> Found Wheel: offset=%d bits=%d", 
                                        wheel_field.bit_offset, wheel_field.bit_count);
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
                        
                        logger.debugf(">>> Found Buttons: offset=%d count=%d bits=%d", 
                                    buttons_field.bit_offset, button_count, buttons_field.bit_count);
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
    
    logger.debugf("\nTotal bits: %d = %d bytes", total_bits, report_size_bytes);
    
    logger.debug("Found: ");
    if (has_buttons) logger.debug("Buttons ");
    if (has_x) logger.debug("X ");
    if (has_y) logger.debug("Y ");
    if (has_wheel) logger.debug("Wheel");
    
    // Check if we have minimum requirements
    if (has_x && has_y) {
        valid = true;
        return true;
    }
    
    // If parsing failed or incomplete, use boot mouse format
    logger.warning("\nMissing X or Y - using boot mouse format");
    setBootMouseFormat();
    return true;  // Return true because we have a valid format now
}

void HIDMouseDescriptorHandler::setBootMouseFormat() {
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
    
    logger.debug("Set boot mouse format");
}

int32_t HIDMouseDescriptorHandler::extractValue(const uint8_t* data, uint16_t bit_offset, uint8_t bit_count,
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

void HIDMouseDescriptorHandler::insertValue(uint8_t* data, uint16_t bit_offset, uint8_t bit_count,
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

bool HIDMouseDescriptorHandler::parseMouseData(const uint8_t* raw_data, uint32_t length, MouseState& state) {
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

bool HIDMouseDescriptorHandler::formatMouseData(const MouseState& state, uint8_t* raw_data, uint32_t& length) {
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

void HIDMouseDescriptorHandler::printInterfaceInfo() {
    logger.debug("\n=== HID Mouse Interface Info ===");
    
    if (interface_index == 0xFF) {
        logger.error("No mouse interface found");
        return;
    }
    
    logger.debugf("Interface Index: %d", interface_index);
    logger.debugf("Interface Number: %d", interface_number);
    logger.debugf("Protocol: %d", interface_protocol);
    
    switch (interface_protocol) {
        case 0: logger.debug(" (None)"); break;
        case 1: logger.debug(" (Keyboard)"); break;
        case 2: logger.debug(" (Mouse)"); break;
        default: logger.debug(" (Unknown)"); break;
    }
    
    logger.debugf("HID Descriptor Length: %d", descriptor_length);
    
    logger.debugf("Endpoint: 0x%02X (%d bytes, interval %dms)", 
                 endpoint_address | 0x80, endpoint_size, endpoint_interval);
    
    logger.debug("State: ");
    switch (handler_state) {
        case HID_STATE_IDLE: logger.debug("IDLE"); break;
        case HID_STATE_WAIT_DESCRIPTOR: logger.debug("WAIT_DESCRIPTOR"); break;
        case HID_STATE_PARSING: logger.debug("PARSING"); break;
        case HID_STATE_READY: logger.debug("READY"); break;
        case HID_STATE_ERROR: logger.error("ERROR"); break;
    }
    
    logger.debugf("Descriptor Valid: %s", valid ? "Yes" : "No");
    
    if (valid && hid_descriptor_size > 0) {
        logger.debugf("Descriptor Size: %d bytes", hid_descriptor_size);
    }
    
    logger.debug("===============================");
}

void HIDMouseDescriptorHandler::printDescriptorInfo() {
    logger.debug("\n=== HID Report Structure ===");
    logger.debugf("Valid: %s", valid ? "Yes" : "No");
    
    if (!valid) return;
    
    logger.debugf("Report ID: %d", report_id);
    logger.debugf("Report Size: %d bytes (%d bits)", report_size_bytes, total_bits);
    
    if (has_buttons) {
        logger.debugf("\nButtons: %d buttons", button_count);
        logger.debugf("  Bit offset: %d", buttons_field.bit_offset);
        logger.debugf("  Total bits: %d", buttons_field.bit_count);
    }
    
    if (has_x) {
        logger.debug("\nX Axis:");
        logger.debugf("  Bit offset: %d (byte %d bit %d)", 
                     x_field.bit_offset, x_field.bit_offset / 8, x_field.bit_offset % 8);
        logger.debugf("  Size: %d bits", x_field.bit_count);
        logger.debugf("  Range: %d to %d", x_field.logical_min, x_field.logical_max);
        logger.debugf("  Type: %s, %s", 
                     x_field.is_relative ? "Relative" : "Absolute",
                     x_field.is_signed ? "Signed" : "Unsigned");
    }
    
    if (has_y) {
        logger.debug("\nY Axis:");
        logger.debugf("  Bit offset: %d (byte %d bit %d)", 
                     y_field.bit_offset, y_field.bit_offset / 8, y_field.bit_offset % 8);
        logger.debugf("  Size: %d bits", y_field.bit_count);
        logger.debugf("  Range: %d to %d", y_field.logical_min, y_field.logical_max);
        logger.debugf("  Type: %s, %s", 
                     y_field.is_relative ? "Relative" : "Absolute",
                     y_field.is_signed ? "Signed" : "Unsigned");
    }
    
    if (has_wheel) {
        logger.debug("\nWheel:");
        logger.debugf("  Bit offset: %d (byte %d bit %d)", 
                     wheel_field.bit_offset, wheel_field.bit_offset / 8, wheel_field.bit_offset % 8);
        logger.debugf("  Size: %d bits", wheel_field.bit_count);
    }
    
    logger.debug("==========================\n");
}

void HIDMouseDescriptorHandler::printMouseState(const MouseState& state) {
    if (debug_enabled) {
        logger.debugf("Mouse: Buttons=0x%02X (", state.buttons);
        
        // Check all possible buttons
        if (state.buttons & 0x01) logger.debug("L");
        if (state.buttons & 0x02) logger.debug("R"); 
        if (state.buttons & 0x04) logger.debug("M");
        if (state.buttons & 0x08) logger.debug("B4");  // Button 4 (thumb back)
        if (state.buttons & 0x10) logger.debug("B5");  // Button 5 (thumb forward)
        if (state.buttons & 0x20) logger.debug("B6");  // Button 6 (if exists)
        if (state.buttons & 0x40) logger.debug("B7");  // Button 7 (if exists)
        if (state.buttons & 0x80) logger.debug("B8");  // Button 8 (if exists)
        
        logger.debugf(") X=%d Y=%d Wheel=%d", state.x, state.y, state.wheel);
    }
}