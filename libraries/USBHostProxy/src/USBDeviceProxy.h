// USBDeviceProxy.h - USB Device Stack for Teensy 4.1 (Polling-based)
#ifndef _USB_DEVICE_PROXY_H_
#define _USB_DEVICE_PROXY_H_

#include <Arduino.h>
#include "usb_dev.h"   // Include this to get the structure definitions

// Define NUM_ENDPOINTS if not already defined
#ifndef NUM_ENDPOINTS
#define NUM_ENDPOINTS 7
#endif

// USB PHY control defines for speed configuration
#define USBPHY_CTRL_ENUTMILEVEL2  ((uint32_t)(1<<14))
#define USBPHY_CTRL_ENUTMILEVEL3  ((uint32_t)(1<<15))
#define USB_PORTSC1_PFSC          ((uint32_t)(1<<24))

// Forward declaration
class USBHostDriver;

// Define endpoint structure (from usb.c) since it's not in usb_dev.h
typedef struct endpoint_struct {
    uint32_t config;
    uint32_t current;
    uint32_t next;
    uint32_t status;
    uint32_t pointer0;
    uint32_t pointer1;
    uint32_t pointer2;
    uint32_t pointer3;
    uint32_t pointer4;
    uint32_t reserved;
    uint32_t setup0;
    uint32_t setup1;
    transfer_t *first_transfer;
    transfer_t *last_transfer;
    void (*callback_function)(transfer_t *completed_transfer);
    uint32_t unused1;
} endpoint_t;

// Setup packet structure
typedef struct {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} setup_packet_t;

// =========================================================================
// Descriptor Cache — eliminates redundant control transfers during enum
// =========================================================================
#define DESC_CACHE_MAX_STRING_ENTRIES  10
#define DESC_CACHE_MAX_STRING_LEN     256
#define DESC_CACHE_MAX_CONFIG_LEN     512
#define DESC_CACHE_MAX_BOS_LEN        512
#define DESC_CACHE_DEVICE_DESC_LEN    18

struct DescriptorCacheEntry {
    uint8_t  data[DESC_CACHE_MAX_CONFIG_LEN]; // reused for config/BOS (512 max)
    uint16_t length;
    bool     valid;
};

struct StringCacheEntry {
    uint8_t  data[DESC_CACHE_MAX_STRING_LEN];
    uint16_t length;
    uint8_t  index;    // string descriptor index
    uint16_t langID;   // wIndex (language ID)
    bool     valid;
};

struct DescriptorCache {
    // Device descriptor (type 0x01) — exactly 18 bytes
    uint8_t  device_desc[DESC_CACHE_DEVICE_DESC_LEN];
    uint16_t device_desc_len;
    bool     device_desc_valid;

    // Configuration descriptor (type 0x02) — full blob up to 512 bytes
    DescriptorCacheEntry config_desc;

    // BOS descriptor (type 0x0F) — up to 512 bytes
    DescriptorCacheEntry bos_desc;

    // String descriptors (type 0x03) — keyed by (index, langID)
    StringCacheEntry string_descs[DESC_CACHE_MAX_STRING_ENTRIES];

    // Identity of the cached device (for invalidation on device change)
    uint16_t cached_vid;
    uint16_t cached_pid;
    bool     identity_valid;

    // Invalidate the entire cache
    void invalidate() {
        device_desc_valid = false;
        device_desc_len = 0;
        config_desc.valid = false;
        config_desc.length = 0;
        bos_desc.valid = false;
        bos_desc.length = 0;
        for (uint8_t i = 0; i < DESC_CACHE_MAX_STRING_ENTRIES; i++) {
            string_descs[i].valid = false;
        }
        identity_valid = false;
        cached_vid = 0;
        cached_pid = 0;
    }

    // Look up a cached descriptor.
    // Returns pointer to cached data and sets out_len, or nullptr on miss.
    const uint8_t* lookup(uint8_t desc_type, uint8_t desc_index,
                          uint16_t wIndex, uint16_t* out_len) const {
        switch (desc_type) {
            case 0x01: // Device descriptor
                if (device_desc_valid) {
                    *out_len = device_desc_len;
                    return device_desc;
                }
                return nullptr;

            case 0x02: // Configuration descriptor
                if (config_desc.valid) {
                    *out_len = config_desc.length;
                    return config_desc.data;
                }
                return nullptr;

            case 0x03: // String descriptor — keyed by (index, langID)
                for (uint8_t i = 0; i < DESC_CACHE_MAX_STRING_ENTRIES; i++) {
                    if (string_descs[i].valid &&
                        string_descs[i].index == desc_index &&
                        string_descs[i].langID == wIndex) {
                        *out_len = string_descs[i].length;
                        return string_descs[i].data;
                    }
                }
                return nullptr;

            case 0x0F: // BOS descriptor
                if (bos_desc.valid) {
                    *out_len = bos_desc.length;
                    return bos_desc.data;
                }
                return nullptr;

            default:
                return nullptr;
        }
    }

    // Store a descriptor in the cache after a successful fetch.
    // For config/BOS: caller must verify full fetch before calling.
    void store(uint8_t desc_type, uint8_t desc_index,
               uint16_t wIndex, const uint8_t* data, uint16_t len) {
        switch (desc_type) {
            case 0x01: // Device descriptor
                if (len > 0 && len <= DESC_CACHE_DEVICE_DESC_LEN) {
                    memcpy(device_desc, data, len);
                    device_desc_len = len;
                    device_desc_valid = true;
                }
                break;

            case 0x02: // Configuration descriptor
                if (len > 0 && len <= DESC_CACHE_MAX_CONFIG_LEN) {
                    memcpy(config_desc.data, data, len);
                    config_desc.length = len;
                    config_desc.valid = true;
                }
                break;

            case 0x03: // String descriptor
                if (len > 0 && len <= DESC_CACHE_MAX_STRING_LEN) {
                    // Find an existing slot or a free slot
                    int8_t free_slot = -1;
                    for (uint8_t i = 0; i < DESC_CACHE_MAX_STRING_ENTRIES; i++) {
                        if (string_descs[i].valid &&
                            string_descs[i].index == desc_index &&
                            string_descs[i].langID == wIndex) {
                            // Update existing entry
                            memcpy(string_descs[i].data, data, len);
                            string_descs[i].length = len;
                            return;
                        }
                        if (!string_descs[i].valid && free_slot < 0) {
                            free_slot = i;
                        }
                    }
                    if (free_slot >= 0) {
                        memcpy(string_descs[free_slot].data, data, len);
                        string_descs[free_slot].length = len;
                        string_descs[free_slot].index = desc_index;
                        string_descs[free_slot].langID = wIndex;
                        string_descs[free_slot].valid = true;
                    }
                    // If no free slot, silently drop (cache is full)
                }
                break;

            case 0x0F: // BOS descriptor
                if (len > 0 && len <= DESC_CACHE_MAX_BOS_LEN) {
                    memcpy(bos_desc.data, data, len);
                    bos_desc.length = len;
                    bos_desc.valid = true;
                }
                break;

            default:
                break;
        }
    }
};

class USBDeviceProxy {
public:
    USBDeviceProxy();

    // Initialize USB hardware (no interrupts!)
    void begin();

    // Set the USB Host Driver reference
    void setUSBHostDriver(USBHostDriver* driver) { hostDriver = driver; }

    // Set device speed configuration (must be called before begin())
    void setDeviceSpeed(bool high_speed) { device_high_speed = high_speed; }

    // Get the actual device speed from host driver
    uint8_t getActualDeviceSpeed() const;

    // Main polling function - MUST be called frequently from loop()
    // Target: >16kHz for 8kHz devices
    void poll();

    // Check connection status
    bool isConnected() const { return device_state >= STATE_DEFAULT; }
    bool isConfigured() const { return device_state == STATE_CONFIGURED; }

    // Get current state (for debugging)
    const char* getStateString() const {
        switch (device_state) {
            case STATE_DETACHED: return "DETACHED";
            case STATE_ATTACHED: return "ATTACHED";
            case STATE_POWERED: return "POWERED";
            case STATE_DEFAULT: return "DEFAULT";
            case STATE_ADDRESS: return "ADDRESS";
            case STATE_CONFIGURED: return "CONFIGURED";
            default: return "UNKNOWN";
        }
    }

    // Get polling statistics
    uint32_t getPollCount() const { return poll_count; }

    // Data endpoint methods
    void sendDataOnEndpoint(uint8_t ep_num, const uint8_t* data, uint32_t length);
    bool isEndpointReady(uint8_t ep_num);

    // Descriptor cache invalidation (called by USBHostDriver on disconnect)
    void invalidateDescriptorCache();

private:
    // USB Device states (USB 2.0 spec chapter 9)
    enum DeviceState {
        STATE_DETACHED,     // Not connected
        STATE_ATTACHED,     // Connected but not powered
        STATE_POWERED,      // Powered but not reset
        STATE_DEFAULT,      // Reset but no address
        STATE_ADDRESS,      // Address assigned
        STATE_CONFIGURED    // Configured and ready
    };
    
    // Control transfer stages
    enum ControlStage {
        CONTROL_IDLE,       // No control transfer active
        CONTROL_SETUP,      // Processing SETUP packet
        CONTROL_DATA_IN,    // Sending data to host
        CONTROL_DATA_OUT,   // Receiving data from host
        CONTROL_STATUS_IN,  // Sending status to host
        CONTROL_STATUS_OUT  // Receiving status from host
    };
    
    // Endpoint information structure
    struct EndpointInfo {
        uint8_t address;        // Endpoint address (with direction bit)
        uint8_t attributes;     // Transfer type and other attributes
        uint16_t maxPacketSize; // Maximum packet size
        uint8_t interval;       // Polling interval
        bool configured;        // Is this endpoint configured?
    };
    
    // Member variables - order must match constructor initialization order!
    // Current state
    DeviceState device_state;
    uint8_t device_address;
    uint8_t configuration_value;
    
    // Hardware state
    bool phy_initialized;
    bool controller_started;
    bool setup_received;
    
    // Control transfer state
    ControlStage control_stage;
    uint16_t setup_data_len;
    
    // Polling statistics
    uint32_t last_poll_time;
    uint32_t poll_count;
    
    // Notification masks - renamed to avoid conflicts
    uint32_t endpoint0_notify_mask;   // Note: not using proxy_ prefix here as it's a member variable
    uint32_t endpointN_notify_mask;
    
    // Setup packet - not in initialization list
    setup_packet_t pending_setup;
    
    // SET_REPORT handling
    setup_packet_t pending_setup_saved;   // Saved setup for SET_REPORT
    uint8_t setup_data_buffer[512];      // Buffer for SET_REPORT data
    bool pending_has_data;               // Flag indicating we have data for pending request
    
    // Endpoint tracking - not in initialization list
    static const uint8_t MAX_PROXY_ENDPOINTS = 16;
    EndpointInfo endpoints[MAX_PROXY_ENDPOINTS];
    uint8_t num_endpoints;
    
    // Endpoint ready states (for data forwarding)
    bool endpoint_ready[MAX_PROXY_ENDPOINTS];
    
    // USB Host Driver reference
    USBHostDriver* hostDriver;
    
    // Device speed configuration
    bool device_high_speed;  // true = 480 Mbps, false = 12 Mbps

    // Descriptor cache — eliminates redundant control transfers during enum
    DescriptorCache desc_cache;

    // Descriptor cache helpers
    bool tryServeCachedDescriptor();  // Returns true if served from cache
    void storeDescriptorInCache(uint8_t desc_type, uint8_t desc_index,
                                uint16_t wIndex, const uint8_t* data,
                                uint16_t len);

    // Initialization functions
    bool initializePHY();
    bool initializeController();
    void initializeEndpoints();
    void startController();
    
    // Polling functions
    void pollControlEndpoint();
    void pollDataEndpoints();
    void handleUSBInterrupt();
    void handleUSBReset();
    void handlePortChange();
    void handleSetupPacket(uint32_t setup0, uint32_t setup1);
    void handleSetAddress();
    
    // Control transfer handling
    void processControlTransfer();
    void sendData(const uint8_t* data, uint32_t length);
    void sendZLP();
    void receiveData(uint8_t* buffer, uint32_t length);
    
    // Endpoint configuration
    void parseConfigurationDescriptor();
    void configureEndpoint(uint8_t addr, uint8_t type, uint16_t maxPacket, uint8_t interval);
    void configureAllEndpoints();
    void handleClearFeature();
    
    // Transfer management (following usb.c pattern)
    void schedule_transfer(endpoint_t *endpoint, uint32_t mask, transfer_t *transfer);
};

#endif // _USB_DEVICE_PROXY_H_