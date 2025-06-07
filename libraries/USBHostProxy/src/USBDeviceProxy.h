// USBDeviceProxy.h - USB Device Stack for Teensy 4.1 (Polling-based)
#ifndef _USB_DEVICE_PROXY_H_
#define _USB_DEVICE_PROXY_H_

#include <Arduino.h>

// Endpoint Queue Head structure (from i.MX RT1062 manual)
typedef struct {
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
} proxy_endpoint_queue_head_t;

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
    
    // Current state
    DeviceState device_state;
    uint8_t device_address;
    uint8_t configuration_value;
    
    // Hardware state
    bool phy_initialized;
    bool controller_started;
    bool setup_received;
    
    // Polling statistics
    uint32_t last_poll_time;
    uint32_t poll_count;
    
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
};

#endif // _USB_DEVICE_PROXY_H_