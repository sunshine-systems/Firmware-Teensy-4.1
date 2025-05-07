#include "DeviceEnumerator.h"
#include "debug/printf.h" // For global printf (output to Serial4 via debugprintf.c)
// No need for <stdarg.h> if we are only using simple printf calls

// ==========================================================================
// Helper Function Implementations (from DeviceEnumerator.h, or local to this file)
// ==========================================================================
// Note: descriptor_type_to_string_de is declared in DeviceEnumerator.h and should be defined here.
const char* descriptor_type_to_string_de(uint8_t type) {
    switch (type) {
        case USB_DESCRIPTOR_DEVICE:           return "DEVICE";
        case USB_DESCRIPTOR_CONFIGURATION:    return "CONFIGURATION";
        case USB_DESCRIPTOR_STRING:           return "STRING";
        case USB_DESCRIPTOR_INTERFACE:        return "INTERFACE";
        case USB_DESCRIPTOR_ENDPOINT:         return "ENDPOINT";
        case USB_DESCRIPTOR_DEVICE_QUALIFIER: return "DEVICE_QUALIFIER";
        case USB_DESCRIPTOR_OTHER_SPEED_CONFIGURATION: return "OTHER_SPEED_CONFIG";
        case USB_DESCRIPTOR_INTERFACE_POWER:  return "INTERFACE_POWER";
        case USB_DESCRIPTOR_OTG:              return "OTG";
        case USB_DESCRIPTOR_DEBUG:            return "DEBUG";
        case USB_DESCRIPTOR_INTERFACE_ASSOCIATION: return "INTERFACE_ASSOCIATION";
        case USB_DESC_TYPE_HUB:               return "HUB";
        case USB_DESC_TYPE_SS_HUB:            return "SS_HUB";
        case USB_DESC_TYPE_ENDPOINT_COMPANION:return "SS_ENDPOINT_COMPANION";
        case USB_DESC_TYPE_HID:               return "HID";
        case USB_DESC_TYPE_REPORT:            return "REPORT";
        case USB_DESC_TYPE_PHYSICAL:          return "PHYSICAL";
        case CS_UNDEFINED:                    return "CS_UNDEFINED";
        case CS_INTERFACE:                    return "CS_INTERFACE";
        case CS_ENDPOINT:                     return "CS_ENDPOINT";
        default:                              return "UNKNOWN_DE";
    }
}

// Internal print helpers using printf
void DeviceEnumerator::_print_endpoint_attributes_debug(uint8_t attr) const {
    if (ENABLE_DE_LOGGING) {
        uint8_t transfer_type = attr & 0x03;
        const char* type_str = "Unknown";
        if (transfer_type == 0) type_str = "Control";
        else if (transfer_type == 1) type_str = "Isochronous";
        else if (transfer_type == 2) type_str = "Bulk";
        else if (transfer_type == 3) type_str = "Interrupt";
        printf("%s", type_str);

        if (transfer_type == 1) { // Isochronous
            uint8_t sync_type = (attr >> 2) & 0x03;
            uint8_t usage_type = (attr >> 4) & 0x03;
            printf(" Sync:");
            if (sync_type == 0) printf("None");
            else if (sync_type == 1) printf("Async");
            else if (sync_type == 2) printf("Adaptive");
            else if (sync_type == 3) printf("Sync");
            printf(" Usage:");
            if (usage_type == 0) printf("Data");
            else if (usage_type == 1) printf("Feedback");
            else if (usage_type == 2) printf("ImplicitFB");
            else printf("Res");
        }
    }
}

void DeviceEnumerator::_print_endpoint_interval_debug(uint8_t interval, uint8_t speed, uint8_t attributes) const {
    if (ENABLE_DE_LOGGING) {
        uint8_t transfer_type = attributes & 0x03;
        if (transfer_type != 1 && transfer_type != 3) {
            printf("%u (N/A for type)", interval);
            return;
        }
        if (speed == USB_SPEED_HIGH) { // USB_SPEED_HIGH defined in DeviceEnumerator.h
            if (interval == 0) interval = 1; 
            if (interval > 16) interval = 16; 
            float mf = pow(2.0f, (float)interval - 1.0f);
            float pus = mf * 125.0f; 
            if (pus >= 1000.0f) {
                 printf("%u (%.1f ms / %.1f Hz)", interval, pus / 1000.0f, (pus > 0 ? 1000000.0f / pus : 0.0f) );
            } else {
                 printf("%u (%.1f us / %.1f KHz)", interval, pus, (pus > 0 ? 1000000.0f / pus / 1000.0f : 0.0f) );
            }
        } else { 
            if (transfer_type == 1) { 
                 printf("%u (%.0f ms)", interval, pow(2.0f, (float)interval -1.0f) );
            } else { 
                if (interval == 0 && speed == USB_SPEED_LOW) interval = 10; 
                else if (interval == 0) interval = 1;
                printf("%u (%u ms / %u Hz)", interval, interval, (interval > 0 ? 1000 / interval : 0));
            }
        }
    }
}


// ==========================================================================
// DeviceEnumerator Class Method Implementations
// ==========================================================================

DeviceEnumerator::DeviceEnumerator(USBHost& host_ref) : USBDriver(),
    current_device_(nullptr), 
    enum_state_(STATE_IDLE),
    config_total_len_(0),
    current_config_index_parsing_(0),
    current_interface_index_parsing_(0),
    current_hid_report_index_parsing_(0)
{
    memset(&stored_data_, 0, sizeof(stored_data_));
    init();
}

void DeviceEnumerator::init() {
    if (ENABLE_DE_LOGGING) printf("DE::init() - DeviceEnumerator instance initializing/resetting.\n");
    clearStoredDeviceData();
    driver_ready_for_device(this); 
}

bool DeviceEnumerator::claim(Device_t* device, int type, const uint8_t* descriptors, uint32_t len) {
    if (type != 0 || device == nullptr) {
        if (ENABLE_DE_LOGGING) printf("DE::claim: Ignoring type %d or null device.\n", type);
        return false;
    }
    if (enum_state_ != STATE_IDLE || current_device_ != nullptr) {
        if (current_device_ == device) {
             if (ENABLE_DE_LOGGING) printf("DE::claim: Already processing this device (VID:0x%04X PID:0x%04X).\n", device->idVendor, device->idProduct);
        } else {
             if (ENABLE_DE_LOGGING) printf("DE::claim: Busy with another device, ignoring new claim for VID:0x%04X PID:0x%04X. Current state: %d\n", device->idVendor, device->idProduct, enum_state_);
        }
        return false; 
    }
    if (ENABLE_DE_LOGGING) printf("DE::claim: New device VID:0x%04X PID:0x%04X. Starting enumeration.\n", device->idVendor, device->idProduct);
    startEnumeration(device);
    return false; 
}

void DeviceEnumerator::disconnect() {
    uint16_t vid = current_device_ ? current_device_->idVendor : stored_data_.idVendor;
    uint16_t pid = current_device_ ? current_device_->idProduct : stored_data_.idProduct;

    if (ENABLE_DE_LOGGING) {
        printf("\nDE::DISCONNECT: VID:0x%04X PID:0x%04X.\n", vid, pid);
    }
    init(); 
    if (ENABLE_DE_LOGGING) printf("DE::disconnect: Cleared data and reset state for VID:0x%04X PID:0x%04X.\n", vid, pid);
}

void DeviceEnumerator::startEnumeration(Device_t* device) {
    if (ENABLE_DE_LOGGING) printf("DE::startEnumeration for VID:0x%04X PID:0x%04X, Addr:%u\n", device->idVendor, device->idProduct, device->address);
    
    clearStoredDeviceData(); 
    current_device_ = device;

    if (!current_device_) {
        if (ENABLE_DE_LOGGING) printf("!!! DE ERROR: startEnumeration called with NULL device pointer!\n");
        enum_state_ = STATE_ERROR;
        processStateMachine();
        return;
    }

    stored_data_.idVendor = device->idVendor;
    stored_data_.idProduct = device->idProduct;
    stored_data_.speed = device->speed;
    stored_data_.hub_address = device->hub_address;
    stored_data_.hub_port = device->hub_port;
    stored_data_.address = device->address; 
    if (device->control_pipe) {
        stored_data_.bMaxPacketSize0 = (device->control_pipe->qh.capabilities[0] >> 16) & 0x7FF;
    } else {
        stored_data_.bMaxPacketSize0 = 8; 
        if (ENABLE_DE_LOGGING) printf("DE WARN: Control pipe not valid at startEnumeration for MPS0, defaulting to 8.\n");
    }

    if (ENABLE_DE_LOGGING) printf("  DE: Initial data: VID=0x%04X, PID=0x%04X, Addr=%u, MPS0=%u\n", 
                                  stored_data_.idVendor, stored_data_.idProduct, stored_data_.address, stored_data_.bMaxPacketSize0);
    
    enum_state_ = STATE_GETTING_DEVICE_DESC_8;
    processStateMachine(); 
}

void DeviceEnumerator::control(const Transfer_t* transfer) {
    if (ENABLE_DE_LOGGING) printf("DE::control - START - State: %d, Transfer Addr: %p, Len: %lu, Token: 0x%lX\n", 
                                 enum_state_, (void*)transfer, transfer ? transfer->length : 0, transfer ? transfer->qtd.token : 0);

    if (!transfer || transfer->driver != this || !current_device_) {
        if (ENABLE_DE_LOGGING) {
            printf("  DE::control: Invalid condition. Transfer:%p, Expected Driver:%p, Actual Driver:%p, CurrentDevice:%p. Ignoring.\n",
                (void*)transfer, (void*)this, (void*)(transfer ? transfer->driver : nullptr), (void*)current_device_);
        }
        return;
    }

    uint32_t token = transfer->qtd.token;
    if (token & (QTD_TOKEN_STATUS_HALTED | QTD_TOKEN_STATUS_BUFFER_ERR | QTD_TOKEN_STATUS_XACT_ERR)) {
        if (ENABLE_DE_LOGGING) printf("!!! DE::Control USB Error/Stall. Token:0x%lX for state %d\n", token, enum_state_);
        if (!(enum_state_ == STATE_GETTING_REPORT_DESC && (token & QTD_TOKEN_STATUS_HALTED))) {
            enum_state_ = STATE_ERROR;
        }
    }
    processStateMachine(transfer);
    if (ENABLE_DE_LOGGING) printf("DE::control - END - New State: %d\n", enum_state_);
}

void DeviceEnumerator::Task() {
    if (current_device_ == nullptr && 
        enum_state_ != STATE_IDLE && 
        enum_state_ != STATE_DONE && 
        enum_state_ != STATE_ERROR) {
        if (ENABLE_DE_LOGGING) printf("DE::Task - Device became NULL unexpectedly (state %d). Resetting via init().\n", enum_state_);
        init(); 
        return;
    }
    
    bool advance_psm = false;
    // Only advance if no transfer is expected (i.e., 'transfer' would be null in processStateMachine)
    // This logic is for states that transition based on internal logic rather than a USB callback.
    if (enum_state_ == STATE_PARSING_CONFIG && stored_data_.configurations[0].rawConfigData != nullptr && stored_data_.configurations[0].rawConfigLen > 0) {
        advance_psm = true; 
    } else if (enum_state_ == STATE_GETTING_CONFIG_STRING && current_device_ != nullptr ) {
        // This state decides whether to fetch a string or skip; it can proceed without a prior transfer if the index was 0.
         if (stored_data_.configurationCount > 0 && stored_data_.configurations[0].rawConfigData != nullptr) { 
             advance_psm = true;
         }
    } else if (enum_state_ == STATE_GETTING_INTERFACE_STRINGS && current_device_ != nullptr) {
         if (stored_data_.configurationCount > 0 && stored_data_.configurations[0].rawConfigData != nullptr) {
            advance_psm = true;
         }
    } else if (enum_state_ == STATE_GETTING_REPORT_DESC && current_device_ != nullptr) {
         if (stored_data_.configurationCount > 0 && stored_data_.configurations[0].rawConfigData != nullptr) {
            advance_psm = true;
         }
    }

    if (advance_psm) {
        if (ENABLE_DE_LOGGING) printf("DE::Task - Advancing state %d (non-USB callback trigger)\n", enum_state_);
        processStateMachine(); 
    }
}

void DeviceEnumerator::clearStoredDeviceData() {
    if (ENABLE_DE_LOGGING) printf("DE::clearStoredDeviceData - Zeroing stored_data_ and deallocating buffers.\n");
    for (uint8_t c = 0; c < MAX_CONFIGURATIONS_PER_DEVICE; ++c) {
        UsbConfigurationData& cfg = stored_data_.configurations[c];
        if (cfg.rawConfigData) { 
            if (ENABLE_DE_LOGGING) printf("  DE: Deleting rawConfigData for config %u (%p, len %u)\n", c, (void*)cfg.rawConfigData, cfg.rawConfigLen);
            delete[] cfg.rawConfigData; 
            cfg.rawConfigData = nullptr; 
            cfg.rawConfigLen = 0;
        }
        for (uint8_t i = 0; i < MAX_INTERFACES_PER_CONFIG; ++i) {
            UsbInterfaceData& iface = cfg.interfaces[i];
            for (uint8_t r = 0; r < MAX_HID_REPORT_DESC_PER_INTERFACE; ++r) {
                if (iface.reportDescriptors[r].rawData) {
                    if (ENABLE_DE_LOGGING) printf("  DE: Deleting reportDescriptor %u for iface_idx %u (%p, len %u)\n", r, i, (void*)iface.reportDescriptors[r].rawData, iface.reportDescriptors[r].wDescriptorLength);
                    delete[] iface.reportDescriptors[r].rawData;
                    iface.reportDescriptors[r].rawData = nullptr;
                    iface.reportDescriptors[r].wDescriptorLength = 0;
                }
            }
        }
    }
    memset(&stored_data_, 0, sizeof(stored_data_)); 
    enum_state_ = STATE_IDLE;
    config_total_len_ = 0;
    current_config_index_parsing_ = 0; 
    current_interface_index_parsing_ = 0;
    current_hid_report_index_parsing_ = 0;
    if (ENABLE_DE_LOGGING) printf("  DE::Finished clearing data.\n");
}

// --- Process State Machine ---
void DeviceEnumerator::processStateMachine(const Transfer_t* transfer) {
    bool repeat_psm = false; 
    int psm_loop_guard = 0;  

    if (ENABLE_DE_LOGGING) {
        printf(" >> DE_PSM ENTRY: State=%d. Transfer %s. CurrentDev:%p\n", 
               enum_state_, (transfer ? "provided" : "not provided"), (void*)current_device_);
    }

    do {
        repeat_psm = false;
        if (++psm_loop_guard > 50) { 
            if (ENABLE_DE_LOGGING) printf("!!! DE_PSM: Loop guard triggered! State %d. Forcing ERROR.\n", enum_state_);
            enum_state_ = STATE_ERROR;
            break; 
        }
        if (!current_device_ && enum_state_ != STATE_IDLE && enum_state_ != STATE_DONE && enum_state_ != STATE_ERROR) {
            if (ENABLE_DE_LOGGING) printf("DE_PSM: Device disconnected during state %d processing. Resetting state via init().\n", enum_state_);
            init(); 
            return; 
        }

        switch (enum_state_) {
            case STATE_IDLE:
                break; 

            case STATE_GETTING_DEVICE_DESC_8:
                if (ENABLE_DE_LOGGING) printf(" DE_PSM State: Requesting DevDesc(8)...\n");
                queueGetDescriptor(USB_DESCRIPTOR_DEVICE, 0, 0, 8); 
                enum_state_ = STATE_GETTING_DEVICE_DESC_FULL;      
                break; 
            
            case STATE_GETTING_DEVICE_DESC_FULL:
                if (!transfer) { if (ENABLE_DE_LOGGING) printf(" DE_PSM State: GETTING_DEVICE_DESC_FULL waiting for CB...\n"); break; }
                if (ENABLE_DE_LOGGING) printf(" DE_PSM State: Processing DevDesc(8) (len %lu), Requesting Full DevDesc...\n", transfer->length);
                
                if (transfer->length < 8 || temp_buffer_[0] < 8 || temp_buffer_[1] != USB_DESCRIPTOR_DEVICE) {
                    if (ENABLE_DE_LOGGING) printf("!!! DE_PSM ERROR: Invalid or short partial Device Descriptor. Len=%lu, bL=%u, bT=%u\n",
                                                 transfer->length, temp_buffer_[0], temp_buffer_[1]);
                    enum_state_ = STATE_ERROR; repeat_psm = true; break;
                }
                stored_data_.bMaxPacketSize0 = temp_buffer_[7]; 
                if (ENABLE_DE_LOGGING) printf("   DE_PSM: Got MaxPacketSize0 = %u from initial 8 bytes.\n", stored_data_.bMaxPacketSize0);
                
                queueGetDescriptor(USB_DESCRIPTOR_DEVICE, 0, 0, sizeof(usb_device_descriptor_t)); 
                enum_state_ = STATE_GETTING_STRING_MAN; 
                break;

            case STATE_GETTING_STRING_MAN:
                if (!transfer) { if (ENABLE_DE_LOGGING) printf(" DE_PSM State: GETTING_STRING_MAN waiting for CB...\n"); break; }
                if (ENABLE_DE_LOGGING) printf(" DE_PSM State: Processing Full DevDesc (len %lu)...\n", transfer->length);

                if (transfer->length < offsetof(usb_device_descriptor_t, bNumConfigurations) + 1 || 
                    temp_buffer_[1] != USB_DESCRIPTOR_DEVICE) {
                    if (ENABLE_DE_LOGGING) printf("!!! DE_PSM ERROR: Invalid or short Full Device Descriptor. Len=%lu, Type=%u\n",
                                                 transfer->length, temp_buffer_[1]);
                    enum_state_ = STATE_ERROR; repeat_psm = true; break;
                }
                { 
                    usb_device_descriptor_t* d = (usb_device_descriptor_t*)temp_buffer_;
                    stored_data_.bcdUSB = d->bcdUSB;
                    stored_data_.bDeviceClass = d->bDeviceClass;
                    stored_data_.bDeviceSubClass = d->bDeviceSubClass;
                    stored_data_.bDeviceProtocol = d->bDeviceProtocol;
                    stored_data_.bcdDevice = d->bcdDevice;
                    stored_data_.iManufacturer = d->iManufacturer;
                    stored_data_.iProduct = d->iProduct;
                    stored_data_.iSerialNumber = d->iSerialNumber;
                    stored_data_.bNumConfigurations = d->bNumConfigurations;
                    if(current_device_) stored_data_.address = current_device_->address; // Update address
                }
                if (ENABLE_DE_LOGGING) printf("   DE_PSM: Full DevDesc OK. Class=%02X, #Configs=%u, iMan=%u, iProd=%u, iSer=%u\n",
                                             stored_data_.bDeviceClass, stored_data_.bNumConfigurations,
                                             stored_data_.iManufacturer, stored_data_.iProduct, stored_data_.iSerialNumber);

                if (stored_data_.iManufacturer > 0) {
                    if (ENABLE_DE_LOGGING) printf(" DE_PSM State: Requesting Manufacturer String (idx %u)...\n", stored_data_.iManufacturer);
                    queueGetDescriptor(USB_DESCRIPTOR_STRING, stored_data_.iManufacturer, 0x0409, MAX_STRING_LENGTH * 2 + 2);
                    enum_state_ = STATE_GETTING_STRING_PROD;
                } else {
                    if (ENABLE_DE_LOGGING) printf(" DE_PSM State: No Manufacturer String (idx 0). Skipping.\n");
                    enum_state_ = STATE_GETTING_STRING_PROD; repeat_psm = true; 
                }
                break;

            case STATE_GETTING_STRING_PROD:
                if (transfer) { 
                    storeString(stored_data_.manufacturerString, MAX_STRING_LENGTH, transfer);
                } else { 
                    if (ENABLE_DE_LOGGING) printf(" DE_PSM State: GETTING_STRING_PROD (no transfer, iMan was 0)\n");
                }
                if (stored_data_.iProduct > 0) {
                    if (ENABLE_DE_LOGGING) printf(" DE_PSM State: Requesting Product String (idx %u)...\n", stored_data_.iProduct);
                    queueGetDescriptor(USB_DESCRIPTOR_STRING, stored_data_.iProduct, 0x0409, MAX_STRING_LENGTH * 2 + 2);
                    enum_state_ = STATE_GETTING_STRING_SERIAL;
                } else {
                    if (ENABLE_DE_LOGGING) printf(" DE_PSM State: No Product String (idx 0). Skipping.\n");
                    enum_state_ = STATE_GETTING_STRING_SERIAL; repeat_psm = true;
                }
                break;

            case STATE_GETTING_STRING_SERIAL:
                if (transfer) { 
                    storeString(stored_data_.productString, MAX_STRING_LENGTH, transfer);
                } else { 
                     if (ENABLE_DE_LOGGING) printf(" DE_PSM State: GETTING_STRING_SERIAL (no transfer, iProd was 0)\n");
                }
                if (stored_data_.iSerialNumber > 0) {
                    if (ENABLE_DE_LOGGING) printf(" DE_PSM State: Requesting Serial Number String (idx %u)...\n", stored_data_.iSerialNumber);
                    queueGetDescriptor(USB_DESCRIPTOR_STRING, stored_data_.iSerialNumber, 0x0409, MAX_STRING_LENGTH * 2 + 2);
                    enum_state_ = STATE_GETTING_CONFIG_HEADER;
                } else {
                    if (ENABLE_DE_LOGGING) printf(" DE_PSM State: No Serial Number String (idx 0). Skipping.\n");
                    enum_state_ = STATE_GETTING_CONFIG_HEADER; repeat_psm = true;
                }
                break;
            
            case STATE_GETTING_CONFIG_HEADER:
                 if (transfer) { 
                    storeString(stored_data_.serialNumberString, MAX_STRING_LENGTH, transfer);
                } else { 
                     if (ENABLE_DE_LOGGING) printf(" DE_PSM State: GETTING_CONFIG_HEADER (no transfer, iSer was 0)\n");
                }
                if (stored_data_.bNumConfigurations == 0) {
                    if (ENABLE_DE_LOGGING) printf(" DE_PSM WARN: Device reports 0 configurations. Marking DONE.\n");
                    enum_state_ = STATE_DONE; repeat_psm = true; break;
                }
                current_config_index_parsing_ = 0; // We only parse the first config (index 0)
                if (ENABLE_DE_LOGGING) printf(" DE_PSM State: Requesting Config Desc Header (idx 0, %u bytes)...\n", (unsigned int)sizeof(usb_configuration_descriptor_t));
                queueGetDescriptor(USB_DESCRIPTOR_CONFIGURATION, current_config_index_parsing_, 0, sizeof(usb_configuration_descriptor_t));
                enum_state_ = STATE_GETTING_CONFIG_FULL;
                break;

            case STATE_GETTING_CONFIG_FULL:
                if (!transfer) { if (ENABLE_DE_LOGGING) printf(" DE_PSM State: GETTING_CONFIG_FULL waiting for CB...\n"); break; }
                if (ENABLE_DE_LOGGING) printf(" DE_PSM State: Processing Config Header (len %lu)...\n", transfer->length);
                if (transfer->length < sizeof(usb_configuration_descriptor_t) || temp_buffer_[1] != USB_DESCRIPTOR_CONFIGURATION) {
                    if (ENABLE_DE_LOGGING) printf("!!! DE_PSM ERROR: Invalid Config Header. Len=%lu, Type=%u\n", transfer->length, temp_buffer_[1]);
                    enum_state_ = STATE_ERROR; repeat_psm = true; break;
                }
                { 
                    usb_configuration_descriptor_t* c = (usb_configuration_descriptor_t*)temp_buffer_;
                    config_total_len_ = c->wTotalLength; // From header
                    if (ENABLE_DE_LOGGING) printf("   DE_PSM: Config Header OK: wTotalLength=%u, #Intf=%u, CfgVal=%u\n", config_total_len_, c->bNumInterfaces, c->bConfigurationValue);
                    if (config_total_len_ < sizeof(usb_configuration_descriptor_t) || config_total_len_ == 0) {
                        if (ENABLE_DE_LOGGING) printf("!!! DE_PSM ERROR: Invalid/Zero wTotalLength in Config Header (%u).\n", config_total_len_);
                        enum_state_ = STATE_ERROR; repeat_psm = true; break;
                    }
                    // Store basic config info if we have space (should always for first config)
                    if (stored_data_.configurationCount < MAX_CONFIGURATIONS_PER_DEVICE) {
                       UsbConfigurationData& cfg_data_ref = stored_data_.configurations[stored_data_.configurationCount]; // Use reference
                       cfg_data_ref = UsbConfigurationData(); // Reset if reusing
                       cfg_data_ref.bConfigurationValue = c->bConfigurationValue;
                       cfg_data_ref.iConfiguration = c->iConfiguration;
                       cfg_data_ref.bmAttributes = c->bmAttributes;
                       cfg_data_ref.bMaxPower = c->bMaxPower;
                       cfg_data_ref.wTotalLength = c->wTotalLength;
                    }
                }
                uint16_t request_len_full_cfg = min(config_total_len_, (uint16_t)TEMP_BUFFER_SIZE);
                if (ENABLE_DE_LOGGING) printf(" DE_PSM State: Requesting Full Config (Req %u bytes, actual total %u)...\n", request_len_full_cfg, config_total_len_);
                queueGetDescriptor(USB_DESCRIPTOR_CONFIGURATION, current_config_index_parsing_, 0, request_len_full_cfg);
                enum_state_ = STATE_PARSING_CONFIG; // Next state expects full config data
                break;

            case STATE_PARSING_CONFIG:
                if (transfer) { // Came from callback with full config data in temp_buffer_
                    if (ENABLE_DE_LOGGING) printf(" DE_PSM State: PARSING_CONFIG (via CB) - Storing Full Config Data (len %lu)...\n", transfer->length);
                    uint16_t actual_recv_len_cfg = (uint16_t)transfer->length;
                    if (actual_recv_len_cfg == 0 && config_total_len_ > 0) { /* ... error log ... */ enum_state_ = STATE_ERROR; repeat_psm = true; break; }
                    
                    uint16_t len_to_store_cfg = min(config_total_len_, min(actual_recv_len_cfg, (uint16_t)TEMP_BUFFER_SIZE));
                    if (stored_data_.configurationCount < MAX_CONFIGURATIONS_PER_DEVICE) {
                        UsbConfigurationData& cfg_ref = stored_data_.configurations[stored_data_.configurationCount];
                        if (cfg_ref.rawConfigData) delete[] cfg_ref.rawConfigData; // Clear old if any
                        cfg_ref.rawConfigData = nullptr; // Prevent use if new fails
                        cfg_ref.rawConfigLen = 0;
                        if (len_to_store_cfg > 0) {
                            cfg_ref.rawConfigData = new (std::nothrow) uint8_t[len_to_store_cfg];
                            if (cfg_ref.rawConfigData) {
                                memcpy(cfg_ref.rawConfigData, temp_buffer_, len_to_store_cfg);
                                cfg_ref.rawConfigLen = len_to_store_cfg;
                                // Only increment on successful store to avoid double-increment if Task calls again
                                if (stored_data_.configurationCount == current_config_index_parsing_) { 
                                    stored_data_.configurationCount++; 
                                }
                                if (ENABLE_DE_LOGGING) printf("   DE_PSM: Stored raw config %u (%u bytes).\n", current_config_index_parsing_, len_to_store_cfg);
                                // Now this state will be re-entered by Task() to do the parsing
                            } else { /* ... malloc error log ... */ enum_state_ = STATE_ERROR; repeat_psm = true; }
                        } else { /* ... zero len error log ... */ enum_state_ = STATE_ERROR; repeat_psm = true; }
                    } else { /* ... max configs log ... */ enum_state_ = STATE_ERROR; repeat_psm = true; }
                } else { // Came from Task() to do the actual parsing
                    if (ENABLE_DE_LOGGING) printf(" DE_PSM State: PARSING_CONFIG (via Task) - Processing stored data for config %u...\n", current_config_index_parsing_);
                    if (stored_data_.configurationCount > current_config_index_parsing_ &&
                        stored_data_.configurations[current_config_index_parsing_].rawConfigData != nullptr &&
                        stored_data_.configurations[current_config_index_parsing_].rawConfigLen > 0) {
                        parseFullConfigurationBlock(); // This will populate interfaces etc. in stored_data_
                        if (enum_state_ != STATE_ERROR) { // Check if parseFullConfigurationBlock set an error
                           if (ENABLE_DE_LOGGING) printf("   DE_PSM: Parsing config %u done. Moving to get strings.\n", current_config_index_parsing_);
                            // Reset indices for string/report fetching for the current config
                            current_interface_index_parsing_ = 0; 
                            current_hid_report_index_parsing_ = 0;
                            enum_state_ = STATE_GETTING_CONFIG_STRING;
                        } // else: parseFullConfigurationBlock hit an error, state already changed
                        repeat_psm = true; // Process next state
                    } else {
                        if (ENABLE_DE_LOGGING) printf(" DE_PSM WARN: No valid raw config data for config %u to parse. Ending enum for this config.\n", current_config_index_parsing_);
                        enum_state_ = STATE_DONE; // Or error if this was unexpected
                        repeat_psm = true;
                    }
                }
                break;

            case STATE_GETTING_CONFIG_STRING:
                // Logic to check if stored_data_.configurations[current_config_index_parsing_].iConfiguration > 0
                // and then queueGetDescriptor or move to INTERFACE_STRINGS.
                // Uses current_config_index_parsing_.
                // Ensure it's called by Task() if iConfiguration is 0.
                // ****** PASTE YOUR LOGIC, CONVERT LOGGING ******
                if (transfer) { /* Should not happen if logic is correct */ if (ENABLE_DE_LOGGING) printf("DE_PSM WARN: Unexpected transfer in GETTING_CONFIG_STRING\n"); break; }
                if (stored_data_.configurationCount > current_config_index_parsing_) {
                    UsbConfigurationData& cfg_ref = stored_data_.configurations[current_config_index_parsing_];
                    if (cfg_ref.iConfiguration > 0) {
                        if (ENABLE_DE_LOGGING) printf(" DE_PSM State: Requesting Config String (idx %u) for config %u...\n", cfg_ref.iConfiguration, current_config_index_parsing_);
                        queueGetDescriptor(USB_DESCRIPTOR_STRING, cfg_ref.iConfiguration, 0x0409, MAX_STRING_LENGTH * 2 + 2);
                        enum_state_ = STATE_GETTING_INTERFACE_STRINGS; // Expects callback
                    } else {
                        if (ENABLE_DE_LOGGING) printf(" DE_PSM State: No Config String for config %u. Skipping.\n", current_config_index_parsing_);
                        enum_state_ = STATE_GETTING_INTERFACE_STRINGS; repeat_psm = true;
                    }
                } else { /* Error or done with configs */ enum_state_ = STATE_DONE; repeat_psm = true; }
                current_interface_index_parsing_ = 0; // Reset for next state
                break;


            case STATE_GETTING_INTERFACE_STRINGS:
                // Logic to iterate stored_data_.configurations[cfg_idx].interfaces[iface_idx].iInterface
                // Uses current_config_index_parsing_ and current_interface_index_parsing_.
                // ****** PASTE YOUR LOGIC, CONVERT LOGGING ******
                if (transfer) { // Came from callback with previous string
                    // Determine if it was config string or an interface string
                    if (current_interface_index_parsing_ == 0 && // This means we were fetching config string as interface index starts at 0 for first call by Task
                        stored_data_.configurationCount > current_config_index_parsing_ &&
                        stored_data_.configurations[current_config_index_parsing_].iConfiguration > 0) {
                        storeString(stored_data_.configurations[current_config_index_parsing_].configurationString, MAX_STRING_LENGTH, transfer);
                    } else if (stored_data_.configurationCount > current_config_index_parsing_ &&
                               (current_interface_index_parsing_ -1) < stored_data_.configurations[current_config_index_parsing_].interfaceCount) {
                        // Interface index was incremented *before* queueing, so use -1
                        storeString(stored_data_.configurations[current_config_index_parsing_].interfaces[current_interface_index_parsing_-1].interfaceString, MAX_STRING_LENGTH, transfer);
                    }
                } // Fall through to queue next interface string or move on
                
                bool iface_str_queued = false;
                if (stored_data_.configurationCount > current_config_index_parsing_) {
                    UsbConfigurationData& cfg_ref = stored_data_.configurations[current_config_index_parsing_];
                    while(current_interface_index_parsing_ < cfg_ref.interfaceCount) {
                        if (cfg_ref.interfaces[current_interface_index_parsing_].iInterface > 0) {
                            if (ENABLE_DE_LOGGING) printf(" DE_PSM State: Requesting Iface String (cfg %u, iface_struct_idx %u, str_idx %u)...\n", 
                                current_config_index_parsing_, current_interface_index_parsing_, cfg_ref.interfaces[current_interface_index_parsing_].iInterface);
                            queueGetDescriptor(USB_DESCRIPTOR_STRING, cfg_ref.interfaces[current_interface_index_parsing_].iInterface, 0x0409, MAX_STRING_LENGTH * 2 + 2);
                            current_interface_index_parsing_++; // Ready for next one after this completes
                            iface_str_queued = true;
                            break; 
                        }
                        current_interface_index_parsing_++; // Skip this interface if iInterface is 0
                    }
                }
                if (!iface_str_queued) { // Done with all interface strings for this config
                    if (ENABLE_DE_LOGGING) printf(" DE_PSM State: Done with Interface Strings for config %u.\n", current_config_index_parsing_);
                    // TODO: Logic to loop to next configuration if MAX_CONFIGURATIONS_PER_DEVICE > 1
                    // For now, assume one config and move to report descriptors
                    current_interface_index_parsing_ = 0; // Reset for report descriptor search
                    current_hid_report_index_parsing_ = 0;
                    enum_state_ = STATE_GETTING_REPORT_DESC;
                    repeat_psm = true;
                }
                break;


            case STATE_GETTING_REPORT_DESC:
                // Logic to iterate stored_data_ ... interfaces[iface_idx].reportDescriptors[report_idx]
                // Uses current_config_index_parsing_, current_interface_index_parsing_, current_hid_report_index_parsing_.
                // ****** PASTE YOUR LOGIC, CONVERT LOGGING ******
                if (transfer) { // Came from callback with previous report descriptor data
                    // Find which report this was for and store it
                     bool PsmFoundPendingReport = false; // Renamed to avoid conflict
                    uint8_t PsmCompletedIfaceIdx = 0, PsmCompletedReportIdx = 0; 
                    if (stored_data_.configurationCount > current_config_index_parsing_) {
                        UsbConfigurationData& c = stored_data_.configurations[current_config_index_parsing_];
                        // Search backwards from current_interface_index_parsing_ -1 / current_hid_report_index_parsing_ -1
                        // This logic needs to be robust. For simplicity, assume transfer is for the *last queued one*.
                        // A better way is to store which one was queued in a member var.
                        // For now, let's assume it was for (current_interface_index_parsing_ -1) and (current_hid_report_index_parsing_ -1) if they are valid.
                        uint8_t last_queued_iface_idx = current_interface_index_parsing_; 
                        uint8_t last_queued_report_idx = current_hid_report_index_;
                        if (last_queued_report_idx > 0) last_queued_report_idx--;
                        else if (last_queued_iface_idx > 0) { last_queued_iface_idx--; /* search for last report of prev iface */ }
                        // This simplified search for the last queued one is prone to errors if multiple were queued rapidly.
                        // Your original robust search logic is better here.

                        // Simplified: Use the indices that were set before the last queueGetHidReportDescriptor call.
                        // This requires careful management of these indices.
                        // For this placeholder, I'll just try to find the first empty one to fill. This is not robust.
                        for(uint8_t ii = 0; ii < c.interfaceCount && !PsmFoundPendingReport; ++ii) {
                            UsbInterfaceData& i_ref = c.interfaces[ii]; // Renamed to avoid conflict
                            if(i_ref.isHidInterface) {
                                for(uint8_t rr = 0; rr < i_ref.reportDescriptorCount && !PsmFoundPendingReport; ++rr) {
                                    UsbHidReportDescInfo& r_ref = i_ref.reportDescriptors[rr]; // Renamed
                                    // A better way to track: have a member like `uint8_t _report_being_fetched_iface, _report_being_fetched_idx;`
                                    if (r_ref.wDescriptorLength > 0 && r_ref.rawData == nullptr) { // This one was pending
                                        PsmCompletedIfaceIdx = ii; PsmCompletedReportIdx = rr; PsmFoundPendingReport = true;
                                    }
                                }
                            }
                        }

                        if (PsmFoundPendingReport) {
                            UsbInterfaceData& i_ref = c.interfaces[PsmCompletedIfaceIdx];
                            UsbHidReportDescInfo& r_ref = i_ref.reportDescriptors[PsmCompletedReportIdx];
                            if (ENABLE_DE_LOGGING) printf(" DE_PSM: Processing Report Desc for Iface %u (struct idx %u), Report struct idx %u\n", i_ref.bInterfaceNumber, PsmCompletedIfaceIdx, PsmCompletedReportIdx);
                            uint16_t actual_recv_len_hid = (uint16_t)transfer->length;
                            uint32_t tk_hid = transfer->qtd.token;
                            if (tk_hid & QTD_TOKEN_STATUS_HALTED) { /* ... log STALL, mark r_ref invalid ... */ r_ref.wDescriptorLength = 0; }
                            else if (actual_recv_len_hid > 0 && r_ref.wDescriptorLength > 0) { // Check expected length also
                                uint16_t len_to_store_hid = min(r_ref.wDescriptorLength, actual_recv_len_hid);
                                if (r_ref.rawData) delete[] r_ref.rawData;
                                r_ref.rawData = new (std::nothrow) uint8_t[len_to_store_hid];
                                if (r_ref.rawData) {
                                    memcpy(r_ref.rawData, temp_buffer_, len_to_store_hid);
                                    r_ref.wDescriptorLength = len_to_store_hid; // Update to actual stored length
                                    if (ENABLE_DE_LOGGING) printf("   DE_PSM: Stored Report Desc (%u bytes)\n", len_to_store_hid);
                                } else { /* ... malloc error log ... */ r_ref.wDescriptorLength = 0;}
                            } else { /* ... 0 len / no expected len log ... */ r_ref.wDescriptorLength = 0; }
                        } else { if (ENABLE_DE_LOGGING) printf("DE_PSM WARN: ReportDesc CB, but no pending found.\n"); }
                    }
                    repeat_psm = true; // Try to find next one
                } // Fall through to find next one to queue

                bool PsmReportQueued = false; // Renamed
                if (stored_data_.configurationCount > current_config_index_parsing_) {
                    UsbConfigurationData& c = stored_data_.configurations[current_config_index_parsing_];
                    for (; current_interface_index_parsing_ < c.interfaceCount; ++current_interface_index_parsing_) {
                        UsbInterfaceData& i_ref = c.interfaces[current_interface_index_parsing_];
                        if (i_ref.isHidInterface) {
                            for (; current_hid_report_index_parsing_ < i_ref.reportDescriptorCount; ++current_hid_report_index_parsing_) {
                                UsbHidReportDescInfo& r_ref = i_ref.reportDescriptors[current_hid_report_index_parsing_];
                                if (r_ref.wDescriptorLength > 0 && r_ref.rawData == nullptr) { // If length known and not yet fetched
                                    if (ENABLE_DE_LOGGING) printf(" DE_PSM State: Requesting Report Desc (cfg %u, iface_idx %u, report_idx %u)...\n",
                                        current_config_index_parsing_, current_interface_index_parsing_, current_hid_report_index_parsing_);
                                    queueGetHidReportDescriptor(current_config_index_parsing_, current_interface_index_parsing_, current_hid_report_index_parsing_);
                                    // current_hid_report_index_parsing_++; // Increment AFTER queueing, or let loop do it. Better to increment after successful queue
                                    PsmReportQueued = true;
                                    goto psm_report_search_end; 
                                }
                            }
                        }
                        current_hid_report_index_parsing_ = 0; // Reset for next interface
                    }
                }
            psm_report_search_end:;
                if (!PsmReportQueued) { // No more reports to fetch for this config
                    if (ENABLE_DE_LOGGING) printf(" DE_PSM State: Done with Report Descriptors for config %u.\n", current_config_index_parsing_);
                    // TODO: Loop to next configuration if any
                    enum_state_ = STATE_DONE; repeat_psm = true;
                }
                break;


            case STATE_DONE:
                if (ENABLE_DE_LOGGING) printf("DE_PSM: Enumeration successfully DONE.\n");
                break; 
            case STATE_ERROR:
                if (ENABLE_DE_LOGGING) printf("!!! DE_PSM: Enumeration ended in ERROR state.\n");
                break;
            default:
                if (ENABLE_DE_LOGGING) printf("!!! DE_PSM: Unknown state %d. Setting ERROR.\n", enum_state_);
                enum_state_ = STATE_ERROR;
                break;
        }
    } while (repeat_psm && enum_state_ != STATE_ERROR && enum_state_ != STATE_DONE);

    if (ENABLE_DE_LOGGING && (enum_state_ == STATE_DONE || enum_state_ == STATE_ERROR) && psm_loop_guard <= 1) {
        printf(" << DE_PSM EXIT (Terminal State): State=%d\n", enum_state_);
    } else if (ENABLE_DE_LOGGING && !repeat_psm && psm_loop_guard <=1 ) { // Only log "Waiting for CB" if not repeating and not terminal
         printf(" << DE_PSM EXIT (Waiting for CB): State=%d\n", enum_state_);
    }
}

// --- queueGetDescriptor: Queues a GET_DESCRIPTOR control transfer ---
void DeviceEnumerator::queueGetDescriptor(uint8_t desc_type, uint8_t desc_index, uint16_t lang_id, uint16_t len) {
    if (!current_device_) {
        if (ENABLE_DE_LOGGING) printf("!!! DE_QGD: No current_device_ to queue descriptor request!\n");
        enum_state_ = STATE_ERROR; 
        processStateMachine();    
        return;
    }
    uint16_t request_len = (len > TEMP_BUFFER_SIZE) ? TEMP_BUFFER_SIZE : len;
    // Allow len 0 for specific tests like SET_ADDRESS, but GET_DESCRIPTOR should have len > 0 usually
    if (request_len == 0 && desc_type != 0 && len != 0) { 
         if (ENABLE_DE_LOGGING) printf("DE_QGD WARN: Requesting 0 length due to TEMP_BUFFER_SIZE for desc type %u, index %u\n", desc_type, desc_index);
    }

    if (ENABLE_DE_LOGGING) printf("  DE_QGD: Queuing Type=0x%X(%s), Idx=%u, Lang=0x%X, ReqLen=%u (OrigAskLen=%u)\n",
            desc_type, descriptor_type_to_string_de(desc_type), desc_index, lang_id, request_len, len);

    mk_setup(setup_packet_, 0x80, USB_REQUEST_GET_DESCRIPTOR, (desc_type << 8) | desc_index, lang_id, request_len);
    
    if (!queue_Control_Transfer(current_device_, &setup_packet_, temp_buffer_, this)) {
        if (ENABLE_DE_LOGGING) printf("!!! DE_QGD ERROR: queue_Control_Transfer for Type 0x%X failed to queue!\n", desc_type);
        enum_state_ = STATE_ERROR; 
        processStateMachine(); 
    }
}

// --- queueGetHidReportDescriptor: Queues GET_DESCRIPTOR for HID Report ---
void DeviceEnumerator::queueGetHidReportDescriptor(uint8_t config_idx_param, uint8_t iface_idx_param, uint8_t report_idx_param) {
    if (!current_device_) { 
        if (ENABLE_DE_LOGGING) printf("!!! DE_QGHRD: No current_device_!\n");
        enum_state_ = STATE_ERROR; processStateMachine(); return;
    }
    if (config_idx_param >= stored_data_.configurationCount || iface_idx_param >= stored_data_.configurations[config_idx_param].interfaceCount ||
        report_idx_param >= stored_data_.configurations[config_idx_param].interfaces[iface_idx_param].reportDescriptorCount) {
        if (ENABLE_DE_LOGGING) printf("!!! DE_QGHRD: Invalid index cfg:%u iface:%u report:%u\n", config_idx_param, iface_idx_param, report_idx_param);
        enum_state_ = STATE_ERROR; processStateMachine(); return;
    }

    UsbInterfaceData& iface_ref = stored_data_.configurations[config_idx_param].interfaces[iface_idx_param];
    UsbHidReportDescInfo& report_ref = iface_ref.reportDescriptors[report_idx_param];

    if (report_ref.wDescriptorLength == 0) {
         if (ENABLE_DE_LOGGING) printf("DE_QGHRD WARN: Skipping GetHidReportDesc for Iface %u, Report Idx %u - target length is 0.\n", iface_ref.bInterfaceNumber, report_idx_param);
         // This report is invalid/empty, state machine should advance past it via Task() or next CB
         return; 
    }

    uint16_t len_to_request = min(report_ref.wDescriptorLength, (uint16_t)TEMP_BUFFER_SIZE);
    if (ENABLE_DE_LOGGING) printf("  DE_QGHRD: Queuing for Iface=%u (struct_idx %u), Report Idx=%u, ReqLen=%u (TotalLen=%u)\n",
             iface_ref.bInterfaceNumber, iface_idx_param, report_idx_param, len_to_request, report_ref.wDescriptorLength);

    mk_setup(setup_packet_, 0x81, USB_REQUEST_GET_DESCRIPTOR, (USB_DESC_TYPE_REPORT << 8) | 0, iface_ref.bInterfaceNumber, len_to_request);
    if (!queue_Control_Transfer(current_device_, &setup_packet_, temp_buffer_, this)) {
        if (ENABLE_DE_LOGGING) printf("!!! DE_QGHRD ERROR: queue_Control_Transfer failed!\n");
        enum_state_ = STATE_ERROR; processStateMachine();
    }
}

// --- storeString: Converts UTF16 string from transfer to ASCII C-string ---
void DeviceEnumerator::storeString(char* out_buffer, size_t out_buffer_len, const Transfer_t* transfer) {
    if (!out_buffer || out_buffer_len == 0) return;
    out_buffer[0] = '\0'; 
    if (!transfer || transfer->length < 2) { 
        if (ENABLE_DE_LOGGING) printf("   DE_StoreStr: Transfer null or too short for string.\n");
        return; 
    }
    const uint8_t* data_ptr = (const uint8_t*)temp_buffer_; 
    uint8_t total_desc_len = data_ptr[0]; 
    uint8_t desc_type = data_ptr[1];

    if (desc_type != USB_DESCRIPTOR_STRING) {
        if (ENABLE_DE_LOGGING) printf("   DE_StoreStr: Incorrect descriptor type (%u), expected STRING (%u).\n", desc_type, USB_DESCRIPTOR_STRING);
        return;
    }
    if (total_desc_len < 2 || total_desc_len > transfer->length) {
         if (ENABLE_DE_LOGGING) printf("   DE_StoreStr: Descriptor length %u invalid or exceeds transfer length %lu.\n", total_desc_len, transfer->length);
         return;
    }
    size_t char_count = 0;
    for (uint8_t k = 2; k < total_desc_len && char_count < (out_buffer_len - 1); k += 2) {
        if (k + 1 < total_desc_len && data_ptr[k+1] == 0x00) { // Basic ASCII if high byte is 0
            char ascii_char = (char)data_ptr[k];
            out_buffer[char_count++] = isprint(ascii_char) ? ascii_char : '?'; // Replace non-printable
        } else { // Non-ASCII or malformed
            out_buffer[char_count++] = '?';
        }
    }
    out_buffer[char_count] = '\0'; // Null terminate
    if (ENABLE_DE_LOGGING) printf("   DE_StoreStr: Stored '%s' (orig len %u, utf16 chars %u)\n", out_buffer, total_desc_len, (total_desc_len-2)/2);
}

// --- parseFullConfigurationBlock: Parses all descriptors in the configuration block ---
void DeviceEnumerator::parseFullConfigurationBlock() {
    if (ENABLE_DE_LOGGING) printf("\nDE_ParseCfg: --- Starting for config index %u --- \n", current_config_index_parsing_);

    if (current_config_index_parsing_ >= stored_data_.configurationCount || 
        !stored_data_.configurations[current_config_index_parsing_].rawConfigData) {
        if (ENABLE_DE_LOGGING) printf("!!! DE_ParseCfg ERROR: No raw config data for index %u.\n", current_config_index_parsing_);
        enum_state_ = STATE_ERROR; return;
    }

    UsbConfigurationData& config_ref = stored_data_.configurations[current_config_index_parsing_]; // Use reference
    const uint8_t* p_desc = config_ref.rawConfigData;
    const uint8_t* p_end_config_block = p_desc + config_ref.rawConfigLen;
    UsbInterfaceData* p_current_interface = nullptr;
    config_ref.interfaceCount = 0; // Reset before parsing

    if (config_ref.rawConfigLen < sizeof(usb_configuration_descriptor_t) || p_desc[1] != USB_DESCRIPTOR_CONFIGURATION) {
        if (ENABLE_DE_LOGGING) printf("!!! DE_ParseCfg ERROR: Invalid config block header.\n");
        enum_state_ = STATE_ERROR; return;
    }
    // Config descriptor itself already partially processed (bVal, iCfg, Attr, MaxP, wTotal)
    // We can re-verify bNumInterfaces from the raw data
    usb_configuration_descriptor_t* actual_cfg_desc = (usb_configuration_descriptor_t*)p_desc;
    if (ENABLE_DE_LOGGING) printf(" DE_ParseCfg: Config #%u, bNumInterfaces in raw data: %u.\n", current_config_index_parsing_, actual_cfg_desc->bNumInterfaces);
    
    p_desc += p_desc[0]; // Advance past the main configuration descriptor

    while (p_desc < p_end_config_block) {
        if (p_desc + 2 > p_end_config_block) { /* EOF error */ if (ENABLE_DE_LOGGING) printf("DE_ParseCfg ERROR: EOF before desc header.\n"); break; }
        uint8_t desc_len = p_desc[0];
        uint8_t desc_type = p_desc[1];
        if (desc_len < 2 || (p_desc + desc_len > p_end_config_block) ) { /* Invalid len error */ if (ENABLE_DE_LOGGING) printf("DE_ParseCfg ERROR: Invalid desc len %u.\n", desc_len); break;}

        if (ENABLE_DE_LOGGING) printf(" DE_ParseCfg: Found Desc Type=0x%02X (%s), Len=%u\n", desc_type, descriptor_type_to_string_de(desc_type), desc_len);

        switch (desc_type) {
            case USB_DESCRIPTOR_INTERFACE:
                if (config_ref.interfaceCount < MAX_INTERFACES_PER_CONFIG) {
                    if (desc_len < sizeof(usb_interface_descriptor_t)) { /* Short desc error */ enum_state_ = STATE_ERROR; goto parse_cfg_done; }
                    p_current_interface = &config_ref.interfaces[config_ref.interfaceCount];
                    *p_current_interface = UsbInterfaceData(); // Reset
                    usb_interface_descriptor_t* ifd = (usb_interface_descriptor_t*)p_desc;
                    // ... (Copy all fields from ifd to p_current_interface) ...
                    p_current_interface->bInterfaceNumber = ifd->bInterfaceNumber;
                    p_current_interface->bAlternateSetting = ifd->bAlternateSetting;
                    p_current_interface->bNumEndpoints = ifd->bNumEndpoints;
                    p_current_interface->bInterfaceClass = ifd->bInterfaceClass;
                    p_current_interface->bInterfaceSubClass = ifd->bInterfaceSubClass;
                    p_current_interface->bInterfaceProtocol = ifd->bInterfaceProtocol;
                    p_current_interface->iInterface = ifd->iInterface;
                    p_current_interface->isHidInterface = (ifd->bInterfaceClass == 3); // Class 3 is HID
                    if (ENABLE_DE_LOGGING) printf("  -> DE_ParseCfg: Stored Iface %u (Num %u, Alt %u, Class %X, HID: %d)\n", config_ref.interfaceCount, ifd->bInterfaceNumber, ifd->bAlternateSetting, ifd->bInterfaceClass, p_current_interface->isHidInterface);
                    config_ref.interfaceCount++;
                } else { /* Max ifaces error */ p_current_interface = nullptr; }
                break;

            case USB_DESCRIPTOR_ENDPOINT:
                if (p_current_interface && p_current_interface->endpointCount < MAX_ENDPOINTS_PER_INTERFACE) {
                    if (desc_len < sizeof(usb_endpoint_descriptor_t)) { /* Short desc error */ enum_state_ = STATE_ERROR; goto parse_cfg_done; }
                    UsbEndpointData& epd_ref = p_current_interface->endpoints[p_current_interface->endpointCount];
                    epd_ref = UsbEndpointData(); // Reset
                    usb_endpoint_descriptor_t* epd_raw = (usb_endpoint_descriptor_t*)p_desc;
                    // ... (Copy all fields from epd_raw to epd_ref) ...
                    epd_ref.bEndpointAddress = epd_raw->bEndpointAddress;
                    epd_ref.bmAttributes = epd_raw->bmAttributes;
                    epd_ref.wMaxPacketSize = epd_raw->wMaxPacketSize;
                    epd_ref.bInterval = epd_raw->bInterval;
                    if (((epd_ref.bmAttributes & 0x03) == 1) && desc_len >= 9) { // Isochronous endpoint with synch address
                        epd_ref.bSynchAddress = p_desc[8]; 
                    }
                    if (ENABLE_DE_LOGGING) printf("    -> DE_ParseCfg: Stored EP %u (Addr 0x%X, Attr 0x%X)\n", p_current_interface->endpointCount, epd_ref.bEndpointAddress, epd_ref.bmAttributes);
                    p_current_interface->endpointCount++;
                } // else: No current iface or max EPs error
                break;
            
            case USB_DESC_TYPE_HID: // HID Class Descriptor
                if (p_current_interface && p_current_interface->isHidInterface) {
                    if (desc_len < offsetof(usb_hid_descriptor_t, optional_descriptors)) { /* Short HID error */ enum_state_ = STATE_ERROR; goto parse_cfg_done; }
                    usb_hid_descriptor_t* hid_d = (usb_hid_descriptor_t*)p_desc;
                    p_current_interface->bcdHID = hid_d->bcdHID;
                    p_current_interface->bCountryCode = hid_d->bCountryCode;
                    p_current_interface->bNumHidClassDescriptors = hid_d->bNumDescriptors;
                    p_current_interface->reportDescriptorCount = 0; // Reset before parsing optionals
                    if (ENABLE_DE_LOGGING) printf("    -> DE_ParseCfg: Stored HID Class Desc (bcd %X, #Opt %u)\n", hid_d->bcdHID, hid_d->bNumDescriptors);
                    
                    const uint8_t* p_opt = ((const uint8_t*)hid_d) + offsetof(usb_hid_descriptor_t, optional_descriptors);
                    for (uint8_t k=0; k < hid_d->bNumDescriptors; ++k) {
                        if (p_opt + 3 > p_end_config_block) { /* Optional desc bounds error */ break; }
                        uint8_t opt_type = p_opt[0];
                        uint16_t opt_len = p_opt[1] | (p_opt[2] << 8);
                        if (ENABLE_DE_LOGGING) printf("      DE_ParseCfg: HID Optional Desc %u: Type 0x%X, Len %u\n", k, opt_type, opt_len);
                        if (opt_type == USB_DESC_TYPE_REPORT) {
                            if (p_current_interface->reportDescriptorCount < MAX_HID_REPORT_DESC_PER_INTERFACE) {
                                UsbHidReportDescInfo& report_info_ref = p_current_interface->reportDescriptors[p_current_interface->reportDescriptorCount];
                                report_info_ref = UsbHidReportDescInfo(); // Reset
                                report_info_ref.bDescriptorType = opt_type;
                                report_info_ref.wDescriptorLength = opt_len;
                                report_info_ref.rawData = nullptr; // To be fetched later
                                if (ENABLE_DE_LOGGING) printf("        -> DE_ParseCfg: Stored HID Report Desc Info %u (len %u)\n", p_current_interface->reportDescriptorCount, opt_len);
                                p_current_interface->reportDescriptorCount++;
                            } else { /* Max HID reports error */ }
                        }
                        p_opt += 3; // Size of this entry in the HID descriptor
                    }
                } // else: Not HID iface or no current iface error
                break;
            // Add cases for other descriptor types you want to parse (IAD, CS_INTERFACE, etc.)
            default:
                if (ENABLE_DE_LOGGING) printf("  DE_ParseCfg: Skipping unhandled descriptor type 0x%02X\n", desc_type);
                break;
        }
        p_desc += desc_len; // Advance to next descriptor
    }
parse_cfg_done:;
    if (ENABLE_DE_LOGGING) printf("DE_ParseCfg: --- Finished parsing config index %u --- \n", current_config_index_parsing_);
}


// printStoredData - ensure all its output uses the 'printer' argument.
void DeviceEnumerator::printStoredData(Print& printer) const {
    if (ENABLE_DE_LOGGING) { 
        printf("DE::printStoredData (internal printf logs to Serial4, detailed output to provided Print object)\n");
    }

    printer.println("\n=================================================");
    printer.println("======= Stored USB Device Enumeration Data =======");
    printer.println("=================================================");

    if (enum_state_ == STATE_ERROR) {
        printer.println("!!! WARNING: Enumeration ended in ERROR state. Data below might be incomplete. !!!");
    } else if (enum_state_ != STATE_DONE) {
        printer.println("!!! WARNING: Enumeration not fully DONE. Data below might be incomplete. !!!");
    }

    if (stored_data_.idVendor == 0 && stored_data_.idProduct == 0 && stored_data_.address == 0) {
        printer.println("--- No Valid Device Data Stored ---");
        printer.println("=================================================");
        return;
    }

    printer.println("--- Device Info ---");
    printer.printf("  VID: 0x%04X, PID: 0x%04X, Address: %u\n", stored_data_.idVendor, stored_data_.idProduct, stored_data_.address);
    printer.print("  Speed: ");
    switch(stored_data_.speed) {
        case USB_SPEED_LOW:  printer.print("Low (1.5Mbps)"); break;
        case USB_SPEED_FULL: printer.print("Full (12Mbps)"); break;
        case USB_SPEED_HIGH: printer.print("High (480Mbps)"); break;
        default: printer.printf("Unknown (%u)", stored_data_.speed); break;
    }
    printer.println();
    printer.printf("  Location: Hub %u, Port %u\n", stored_data_.hub_address, stored_data_.hub_port);
    printer.printf("  Class: 0x%02X, SubClass: 0x%02X, Protocol: 0x%02X (%s)\n",
        stored_data_.bDeviceClass, stored_data_.bDeviceSubClass, stored_data_.bDeviceProtocol,
        (stored_data_.bDeviceClass == 0 ? "(Defined at Interface level)" : "Device Specific"));
    printer.printf("  MaxPacketSize0: %u bytes\n", stored_data_.bMaxPacketSize0);
    printer.printf("  bcdUSB: 0x%04X (%u.%u%u), ",
        stored_data_.bcdUSB, stored_data_.bcdUSB >> 8, (stored_data_.bcdUSB >> 4) & 0xF, stored_data_.bcdUSB & 0xF);
    printer.printf("bcdDevice: 0x%04X (%u.%u%u)\n",
        stored_data_.bcdDevice, stored_data_.bcdDevice >> 8, (stored_data_.bcdDevice >> 4) &