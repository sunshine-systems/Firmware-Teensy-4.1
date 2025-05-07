#include "DeviceEnumerator.h"
#include "debug/printf.h" 
#include <math.h> // For pow in print_endpoint_interval if you re-enable full version

// --- Static Dummy Report Descriptors ---
const uint8_t DeviceEnumerator::dummy_kbd_report_desc[] = { 
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x75, 0x01, 0x95, 0x08, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,
    0x15, 0x00, 0x25, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x03, 0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x03, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0
};
const uint16_t DeviceEnumerator::dummy_kbd_report_desc_len = sizeof(DeviceEnumerator::dummy_kbd_report_desc);

const uint8_t DeviceEnumerator::dummy_mouse_report_desc[] = { 
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03,
    0x81, 0x06, 0xC0, 0xC0
};
const uint16_t DeviceEnumerator::dummy_mouse_report_desc_len = sizeof(DeviceEnumerator::dummy_mouse_report_desc);


DeviceEnumerator::DeviceEnumerator(USBHost& host_ref) : USBDriver(),
    current_device_(nullptr), 
    enum_state_(STATE_IDLE),
    data_populated_this_claim_(false)
{
    memset(&stored_data_, 0, sizeof(stored_data_));
    init();
}

void DeviceEnumerator::init() {
    if (ENABLE_DE_LOGGING) printf("DE_Dummy::init()\n");
    clearStoredDeviceData(); 
    driver_ready_for_device(this); 
}

bool DeviceEnumerator::claim(Device_t* device, int type, const uint8_t* descriptors, uint32_t len) {
    if (ENABLE_DE_LOGGING) printf("DE_Dummy::claim() ENTERED. Type: %d, Device: %p, VID:0x%04X, PID:0x%04X\n", 
                                   type, (void*)device, device ? device->idVendor : 0, device ? device->idProduct : 0);
    if (type != 0 || device == nullptr) {
        if (ENABLE_DE_LOGGING) printf("DE_Dummy::claim: Ignoring type %d or null device.\n", type);
        return false;
    }
    if (current_device_ != nullptr) { 
        if (ENABLE_DE_LOGGING) printf("DE_Dummy::claim: Already 'handling' a device VID:0x%04X.\n", current_device_->idVendor);
        return false; 
    }

    if (ENABLE_DE_LOGGING) printf("DE_Dummy::claim: \"Claiming\" device VID:0x%04X PID:0x%04X\n", device->idVendor, device->idProduct);
    current_device_ = device; 
    enum_state_ = STATE_PROCESSING_STATIC;
    data_populated_this_claim_ = false; 
    Task(); // Immediately attempt to populate and set to DONE
    return false; 
}

void DeviceEnumerator::disconnect() {
    if (ENABLE_DE_LOGGING) printf("DE_Dummy::disconnect: Device VID:0x%04X PID:0x%04X\n", 
                                   current_device_ ? current_device_->idVendor : 0, 
                                   current_device_ ? current_device_->idProduct : 0);
    init(); 
}

void DeviceEnumerator::control(const Transfer_t* transfer) {
    if (ENABLE_DE_LOGGING) printf("DE_Dummy::control - Called (NOP for dummy version)\n");
}

void DeviceEnumerator::Task() {
    if (enum_state_ == STATE_PROCESSING_STATIC && current_device_ != nullptr) {
        if (!data_populated_this_claim_) {
            if (ENABLE_DE_LOGGING) printf("DE_Dummy::Task - Populating static composite data.\n");
            populateStaticDataWithCompositeDevice();
            data_populated_this_claim_ = true;
        }
        enum_state_ = STATE_DONE;
        if (ENABLE_DE_LOGGING) printf("DE_Dummy::Task - Moved to STATE_DONE.\n");
    }
}

void DeviceEnumerator::clearStoredDeviceData() {
    if (ENABLE_DE_LOGGING) printf("DE_Dummy::clearStoredDeviceData\n");
    memset(&stored_data_, 0, sizeof(stored_data_));
    enum_state_ = STATE_IDLE;
    if (enum_state_ == STATE_IDLE) current_device_ = nullptr; 
    data_populated_this_claim_ = false;
}

void DeviceEnumerator::populateStaticDataWithCompositeDevice() {
    if (ENABLE_DE_LOGGING) printf("DE_Dummy::populateStaticDataWithCompositeDevice - Setting hardcoded values.\n");
    
    // Clear previous data first
    memset(&stored_data_, 0, sizeof(stored_data_));

    // Device Descriptor Info
    stored_data_.idVendor = 0x16C0; 
    stored_data_.idProduct = 0x04CD; 
    stored_data_.bcdUSB = 0x0200;    
    stored_data_.bDeviceClass = 0xEF; 
    stored_data_.bDeviceSubClass = 0x02; 
    stored_data_.bDeviceProtocol = 0x01; 
    stored_data_.bMaxPacketSize0 = 64;
    stored_data_.bcdDevice = 0x0110; 
    stored_data_.iManufacturer = 1;
    stored_data_.iProduct = 2;
    stored_data_.iSerialNumber = 3;
    stored_data_.bNumConfigurations = 1;

    if (current_device_) { 
        stored_data_.speed = current_device_->speed;
        stored_data_.address = current_device_->address;
        stored_data_.hub_address = current_device_->hub_address;
        stored_data_.hub_port = current_device_->hub_port;
    } else { 
        stored_data_.speed = USB_SPEED_FULL; stored_data_.address = 1; 
    }

    strncpy(stored_data_.manufacturerString, "Teensy Dummy Corp.", MAX_STRING_LENGTH - 1);
    stored_data_.manufacturerString[MAX_STRING_LENGTH - 1] = '\0';
    strncpy(stored_data_.productString, "Proxy Kbd+Mouse", MAX_STRING_LENGTH - 1);
    stored_data_.productString[MAX_STRING_LENGTH - 1] = '\0';
    strncpy(stored_data_.serialNumberString, "SN_DUMMY001", MAX_STRING_LENGTH - 1);
    stored_data_.serialNumberString[MAX_STRING_LENGTH - 1] = '\0';

    // Configuration 0
    stored_data_.configurationCount = 1;
    UsbConfigurationData& cfg = stored_data_.configurations[0];
    cfg.bConfigurationValue = 1;
    cfg.iConfiguration = 0; 
    cfg.bmAttributes = 0xA0; 
    cfg.bMaxPower = 50;      

    // Interface 0: Keyboard
    cfg.interfaceCount = 2; 
    UsbInterfaceData& iface_kbd = cfg.interfaces[0];
    iface_kbd.bInterfaceNumber = 0;
    iface_kbd.bInterfaceClass = 0x03; 
    iface_kbd.bInterfaceSubClass = 0x01; 
    iface_kbd.bInterfaceProtocol = 0x01; 
    iface_kbd.iInterface = 4; 
    strncpy(iface_kbd.interfaceString, "Keyboard Interface", MAX_STRING_LENGTH -1);
    iface_kbd.interfaceString[MAX_STRING_LENGTH - 1] = '\0';
    iface_kbd.isHidInterface = true;
    iface_kbd.bcdHID = 0x0111; 
    iface_kbd.bCountryCode = 0;
    iface_kbd.bNumHidClassDescriptors = 1; 
    iface_kbd.reportDescriptorCount = 1;
    iface_kbd.reportDescriptors[0].bDescriptorType = USB_DESC_TYPE_REPORT;
    iface_kbd.reportDescriptors[0].wDescriptorLength = dummy_kbd_report_desc_len;
    iface_kbd.reportDescriptors[0].rawData = dummy_kbd_report_desc; // Point to static const data

    iface_kbd.endpointCount = 1;
    UsbEndpointData& ep_kbd = iface_kbd.endpoints[0];
    ep_kbd.bEndpointAddress = 0x81; 
    ep_kbd.bmAttributes = 0x03;     
    ep_kbd.wMaxPacketSize = 8;      
    ep_kbd.bInterval = 10;          

    // Interface 1: Mouse
    UsbInterfaceData& iface_mouse = cfg.interfaces[1];
    iface_mouse.bInterfaceNumber = 1;
    iface_mouse.bInterfaceClass = 0x03; 
    iface_mouse.bInterfaceSubClass = 0x01; 
    iface_mouse.bInterfaceProtocol = 0x02; 
    iface_mouse.iInterface = 5; 
    strncpy(iface_mouse.interfaceString, "Mouse Interface", MAX_STRING_LENGTH-1);
    iface_mouse.interfaceString[MAX_STRING_LENGTH-1] = '\0';
    iface_mouse.isHidInterface = true;
    iface_mouse.bcdHID = 0x0111; 
    iface_mouse.bCountryCode = 0;
    iface_mouse.bNumHidClassDescriptors = 1;
    iface_mouse.reportDescriptorCount = 1;
    iface_mouse.reportDescriptors[0].bDescriptorType = USB_DESC_TYPE_REPORT;
    iface_mouse.reportDescriptors[0].wDescriptorLength = dummy_mouse_report_desc_len;
    iface_mouse.reportDescriptors[0].rawData = dummy_mouse_report_desc; // Point to static const data

    iface_mouse.endpointCount = 1;
    UsbEndpointData& ep_mouse = iface_mouse.endpoints[0];
    ep_mouse.bEndpointAddress = 0x82; 
    ep_mouse.bmAttributes = 0x03;     
    ep_mouse.wMaxPacketSize = 8;      
    ep_mouse.bInterval = 10;          

    if (ENABLE_DE_LOGGING) printf("DE_Dummy: Static composite (Kbd+Mouse) data populated.\n");
}

void DeviceEnumerator::printStoredData(Print& printer) const {
    #ifdef printf
    #undef printf
    #define PRINTF_MACRO_WAS_ACTIVE_IN_PRINTSTOREDDATA 
    #endif

    if (ENABLE_DE_LOGGING) { 
        printf_debug("DE_Dummy::printStoredData (internal logs to Serial4, detailed output to Print object)\n");
    }

    printer.println("\n=================================================");
    printer.println("======= DUMMY Stored USB Device Data ======="); // Indicate DUMMY
    printer.println("=================================================");

    if (enum_state_ == STATE_ERROR) {
        printer.println("!!! WARNING: Enumerator in ERROR state. !!!");
    } else if (enum_state_ != STATE_DONE && !(current_device_ && data_populated_this_claim_)) {
         // If not done and not actively processing a claimed device with populated data
        printer.println("!!! WARNING: Enumeration not fully DONE or no data. !!!");
    }


    if (stored_data_.idVendor == 0 && stored_data_.idProduct == 0 && stored_data_.address == 0) {
        printer.println("--- No Valid Device Data Stored (or cleared) ---");
        goto print_end_dummy; 
    }

    printer.println("--- Device Info (Static/Dummy) ---");
    printer.printf("  VID: 0x%04X, PID: 0x%04X, Address: %u\n", stored_data_.idVendor, stored_data_.idProduct, stored_data_.address);
    printer.print("  Speed: ");
    switch(stored_data_.speed) {
        case USB_SPEED_LOW:  printer.print("Low (1.5Mbps)"); break;
        case USB_SPEED_FULL: printer.print("Full (12Mbps)"); break;
        case USB_SPEED_HIGH: printer.print("High (480Mbps)"); break;
        default: printer.printf("Unknown (%u)", stored_data_.speed); break;
    }
    printer.println();
    printer.printf("  Class:0x%02X Sub:0x%02X Proto:0x%02X\n", stored_data_.bDeviceClass, stored_data_.bDeviceSubClass, stored_data_.bDeviceProtocol);
    printer.printf("  MaxPktSize0: %u, bcdDevice: 0x%04X\n", stored_data_.bMaxPacketSize0, stored_data_.bcdDevice);
    printer.printf("  #Configs: %u (Stored: %u)\n", stored_data_.bNumConfigurations, stored_data_.configurationCount);
    printer.printf("  iMfr: %u -> '%s'\n", stored_data_.iManufacturer, stored_data_.manufacturerString);
    printer.printf("  iPrd: %u -> '%s'\n", stored_data_.iProduct, stored_data_.productString);
    printer.printf("  iSer: %u -> '%s'\n", stored_data_.iSerialNumber, stored_data_.serialNumberString);

    for (uint8_t c = 0; c < stored_data_.configurationCount; ++c) {
        const UsbConfigurationData& cfg = stored_data_.configurations[c];
        printer.printf("\n  --- Config %u ---\n", c);
        printer.printf("    bCfgVal: %u, iCfg: %u ('%s')\n", cfg.bConfigurationValue, cfg.iConfiguration, cfg.configurationString);
        printer.printf("    Attr: 0x%02X, MaxPwr: %umA\n", cfg.bmAttributes, cfg.bMaxPower * 2);
        printer.printf("    #Ifaces: %u\n", cfg.interfaceCount);

        for (uint8_t i = 0; i < cfg.interfaceCount; ++i) {
            const UsbInterfaceData& iface = cfg.interfaces[i];
            printer.printf("\n    --- Iface %u (Num %u, Alt %u) ---\n", i, iface.bInterfaceNumber, iface.bAlternateSetting);
            printer.printf("      Cls:0x%02X Sub:0x%02X Proto:0x%02X iStr:%u ('%s')\n",
                iface.bInterfaceClass, iface.bInterfaceSubClass, iface.bInterfaceProtocol, iface.iInterface, iface.interfaceString);
            printer.printf("      #EPs:%u %s\n", iface.endpointCount, (iface.isHidInterface ? "[HID]" : ""));

            if (iface.isHidInterface) {
                printer.printf("        HID: bcd:0x%04X Country:%u #ClassDesc:%u ReportsStored:%u\n",
                    iface.bcdHID, iface.bCountryCode, iface.bNumHidClassDescriptors, iface.reportDescriptorCount);
                for (uint8_t r_idx = 0; r_idx < iface.reportDescriptorCount; ++r_idx) { // Renamed loop var
                    const UsbHidReportDescInfo& report = iface.reportDescriptors[r_idx];
                    printer.printf("        ReportDesc %u: Type=0x%02X Len=%u\n", r_idx, report.bDescriptorType, report.wDescriptorLength);
                    if (report.rawData && report.wDescriptorLength > 0 && ENABLE_DE_LOGGING) {
                        printf_debug("          DE_PStoreData: Raw Report Desc %u for Iface %u (on Serial4):\n            ", r_idx, iface.bInterfaceNumber);
                        for (uint16_t k = 0; k < report.wDescriptorLength; ++k) {
                            if (k > 0 && k % 16 == 0) printf_debug("\n            ");
                            else if (k > 0 && k % 8 == 0) printf_debug(" ");
                            printf_debug("%02X ", report.rawData[k]);
                        }
                        printf_debug("\n");
                    }
                }
            }
            if (iface.endpointCount > 0) printer.println("      --- Endpoints ---");
            else printer.println("      (No Endpoints for this Iface)");
            
            for (uint8_t e_idx = 0; e_idx < iface.endpointCount; ++e_idx) { // Renamed loop var
                const UsbEndpointData& ep = iface.endpoints[e_idx];
                printer.printf("        EP %u: Addr=0x%02X Attr=0x%02X(", e_idx, ep.bEndpointAddress, ep.bmAttributes);
                // For output to 'printer', we'd call a helper that takes Print&
                // _print_endpoint_attributes_to_printer(printer, ep.bmAttributes); 
                // Or inline:
                uint8_t transfer_type_ep = ep.bmAttributes & 0x03; // Renamed
                const char* type_str_ep = "Unk"; // Renamed
                if (transfer_type_ep == 0) type_str_ep = "Ctrl";
                else if (transfer_type_ep == 1) type_str_ep = "Iso";
                else if (transfer_type_ep == 2) type_str_ep = "Bulk";
                else if (transfer_type_ep == 3) type_str_ep = "Int";
                printer.print(type_str_ep);
                printer.print(") MaxPkt="); printer.print(ep.wMaxPacketSize & 0x7FF);
                printer.print(", Interval="); printer.print(ep.bInterval);
                if (((ep.bmAttributes & 0x03) == 1) && ep.bSynchAddress != 0) { 
                    printer.printf(", SyncAddr=%u", ep.bSynchAddress);
                }
                printer.println();
            }
        }
    }
print_end_dummy:;
    printer.println("\n=================================================");
    printer.println("======= End of DUMMY Stored Data Printout =======");
    printer.println("=================================================");

    #ifdef PRINTF_MACRO_WAS_ACTIVE_IN_PRINTSTOREDDATA
    #define printf(...) printf_debug(__VA_ARGS__) 
    #undef PRINTF_MACRO_WAS_ACTIVE_IN_PRINTSTOREDDATA
    #endif
}

// --- Dummy/NOP Implementations for Full Enumerator Methods ---
// These are declared in the .h file, so they need a body, even if empty for the dummy.
void DeviceEnumerator::processStateMachine(const Transfer_t* transfer) {
    if (ENABLE_DE_LOGGING) {
        // Only log if not already done (which Task() would have set if it was the final step)
        if (enum_state_ != STATE_DONE && enum_state_ != STATE_ERROR) {
             printf("DE_Dummy::processStateMachine (State: %d) - NOP for dummy version (transfer %p).\n", enum_state_, (void*)transfer);
        }
    }
}
void DeviceEnumerator::queueGetDescriptor(uint8_t dt, uint8_t di, uint16_t li, uint16_t l) {
    if (ENABLE_DE_LOGGING) printf("DE_Dummy::queueGetDescriptor - NOP.\n");
}
void DeviceEnumerator::queueGetHidReportDescriptor(uint8_t c, uint8_t i, uint8_t r) {
    if (ENABLE_DE_LOGGING) printf("DE_Dummy::queueGetHidReportDesc - NOP.\n");
}
void DeviceEnumerator::storeString(char* ob, size_t obl, const Transfer_t* t) {
    if (ENABLE_DE_LOGGING) printf("DE_Dummy::storeString - NOP (data is static).\n");
    if(ob && obl > 0) ob[0] = '\0';
}
void DeviceEnumerator::parseFullConfigurationBlock() {
    if (ENABLE_DE_LOGGING) printf("DE_Dummy::parseFullConfigurationBlock - NOP (data is static).\n");
}