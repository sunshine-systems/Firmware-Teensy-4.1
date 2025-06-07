// USBDeviceProxy.h - USB Device Stack for Teensy 4.1 (Polling-based)
#ifndef _USB_DEVICE_PROXY_H_
#define _USB_DEVICE_PROXY_H_

#include <Arduino.h>
#include "usb_dev.h"   // Include this to get the structure definitions

// Define NUM_ENDPOINTS if not already defined
#ifndef NUM_ENDPOINTS
#define NUM_ENDPOINTS 7
#endif

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

class USBDeviceProxy {
public:
    USBDeviceProxy();
    
    // Initialize USB hardware (no interrupts!)
    void begin();
    
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
    
    // Initialization functions
    bool initializePHY();
    bool initializeController();
    void initializeEndpoints();
    void startController();
    
    // Polling functions
    void pollControlEndpoint();
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
    
    // Transfer management (following usb.c pattern)
    void schedule_transfer(endpoint_t *endpoint, uint32_t mask, transfer_t *transfer);
};

#endif // _USB_DEVICE_PROXY_H_