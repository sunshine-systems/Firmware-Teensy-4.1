#include "usb_dev.h"
#define USB_DESC_LIST_DEFINE
#include "usb_desc.h"
#include "usb_serial.h"
#include "usb_seremu.h"
#include "usb_rawhid.h"
#include "usb_keyboard.h"
#include "usb_mouse.h"
#include "usb_joystick.h"
#include "usb_flightsim.h"
#include "usb_touch.h"
#include "usb_midi.h"
#include "usb_audio.h"
#include "usb_mtp.h"
#include "core_pins.h"
#include "avr/pgmspace.h"
#include <string.h>
#include "debug/printf.h"

// Include our USB Host wrapper (C-compatible interface)
#include "../../libraries/USBHostProxy/src/usb_host_wrapper.h"

#if defined(NUM_ENDPOINTS)

// Type definitions need to come first
typedef struct endpoint_struct endpoint_t;

struct endpoint_struct {
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
};

typedef union {
    struct {
        union {
            struct {
                uint8_t bmRequestType;
                uint8_t bRequest;
            };
            uint16_t wRequestAndType;
        };
        uint16_t wValue;
        uint16_t wIndex;
        uint16_t wLength;
    };
    struct {
        uint32_t word1;
        uint32_t word2;
    };
    uint64_t bothwords;
} setup_t;

// Proxy state
static volatile bool proxy_device_connected = false;
static volatile bool proxy_enumeration_complete = false;
static uint8_t proxy_config_value = 1;

// Descriptor cache
static uint8_t device_descriptor_cache[18];
static uint8_t config_descriptor_cache[512];  // Larger buffer for complex devices
static uint16_t config_descriptor_length = 0;
static uint8_t string_descriptor_cache[256];

// Endpoint mapping for data transfers
typedef struct {
    uint8_t ep_num;
    uint8_t ep_dir;  // 0=OUT, 1=IN
    uint16_t max_packet_size;
    uint8_t ep_type;  // 0=control, 1=iso, 2=bulk, 3=interrupt
    uint8_t interval;
    transfer_t *active_transfers[2];  // Double buffering
    uint8_t active_buffer;
    uint8_t buffer[2][512] __attribute__ ((aligned(32)));  // Double buffers
    volatile bool busy[2];
} endpoint_proxy_t;

static endpoint_proxy_t proxy_endpoints[8];  // Support up to 8 endpoints
static uint8_t num_proxy_endpoints = 0;

// Control transfer state
static volatile bool control_proxy_busy = false;
static uint8_t control_proxy_buffer[512] __attribute__ ((aligned(32)));
static setup_t pending_setup;

// Standard USB variables
endpoint_t endpoint_queue_head[(NUM_ENDPOINTS+1)*2] __attribute__ ((used, aligned(4096), section(".endpoint_queue") ));
transfer_t endpoint0_transfer_data __attribute__ ((used, aligned(32)));
transfer_t endpoint0_transfer_ack  __attribute__ ((used, aligned(32)));

static uint32_t endpoint0_notify_mask=0;
static uint32_t endpointN_notify_mask=0;
volatile uint8_t usb_configuration = 0;
volatile uint8_t usb_high_speed = 0;
static uint8_t endpoint0_buffer[8];
static uint8_t reply_buffer[8];
static uint8_t usb_reboot_timer = 0;
static uint8_t sof_usage = 0;

extern uint8_t usb_descriptor_buffer[];
extern const uint8_t usb_config_descriptor_480[];
extern const uint8_t usb_config_descriptor_12[];

void (*usb_timer0_callback)(void) = NULL;
void (*usb_timer1_callback)(void) = NULL;

// Forward declarations
void usb_isr(void);
static void endpoint0_setup(uint64_t setupdata);
static void endpoint0_transmit(const void *data, uint32_t len, int notify);
static void endpoint0_receive(void *data, uint32_t len, int notify);
static void endpoint0_complete(void);
static void run_callbacks(endpoint_t *ep);
static void proxy_in_complete(transfer_t *transfer);
static void proxy_out_complete(transfer_t *transfer);
static bool forward_control_request(setup_t *setup, uint8_t *data, uint16_t *len);
static void check_device_connection(void);
static void schedule_transfer(endpoint_t *endpoint, uint32_t epmask, transfer_t *transfer);

FLASHMEM void usb_init(void)
{
    // Wait for USB Host Driver to be ready
    printf("USB Proxy: Waiting for host driver...\n");
    uint32_t timeout = 5000;  // 5 second timeout
    while (!usb_host_is_ready() && timeout > 0) {
        delay(10);
        timeout -= 10;
    }
    
    if (usb_host_is_ready()) {
        proxy_device_connected = true;
        printf("USB Proxy: Device connected, starting enumeration\n");
        
        // Pre-fetch device descriptor
        setup_t get_device_desc;
        get_device_desc.bmRequestType = 0x80;
        get_device_desc.bRequest = 0x06;
        get_device_desc.wValue = 0x0100;
        get_device_desc.wIndex = 0;
        get_device_desc.wLength = 18;
        uint16_t actual_len = 0;
        if (usb_host_control_transfer(
            get_device_desc.bmRequestType,
            get_device_desc.bRequest,
            get_device_desc.wValue,
            get_device_desc.wIndex,
            get_device_desc.wLength,
            device_descriptor_cache,
            &actual_len,
            1000)) {
            printf("USB Proxy: Got device descriptor, VID=%04X PID=%04X\n",
                device_descriptor_cache[8] | (device_descriptor_cache[9] << 8),
                device_descriptor_cache[10] | (device_descriptor_cache[11] << 8));
        }
    } else {
        printf("USB Proxy: No device connected or timeout\n");
    }
    
    // Standard USB initialization continues...
    PMU_REG_3P0 = PMU_REG_3P0_OUTPUT_TRG(0x0F) | PMU_REG_3P0_BO_OFFSET(6)
        | PMU_REG_3P0_ENABLE_LINREG;

    usb_init_serialnumber();

    CCM_CCGR6 |= CCM_CCGR6_USBOH3(CCM_CCGR_ON);
    
    USB1_BURSTSIZE = 0x0404;
    
    if ((USBPHY1_PWD & (USBPHY_PWD_RXPWDRX | USBPHY_PWD_RXPWDDIFF | USBPHY_PWD_RXPWD1PT1
      | USBPHY_PWD_RXPWDENV | USBPHY_PWD_TXPWDV2I | USBPHY_PWD_TXPWDIBIAS
      | USBPHY_PWD_TXPWDFS)) || (USB1_USBMODE & USB_USBMODE_CM_MASK)) {
        USBPHY1_CTRL_SET = USBPHY_CTRL_SFTRST;
        USB1_USBCMD |= USB_USBCMD_RST;
        int count=0;
        while (USB1_USBCMD & USB_USBCMD_RST) count++;
        NVIC_CLEAR_PENDING(IRQ_USB1);
        USBPHY1_CTRL_CLR = USBPHY_CTRL_SFTRST;
        printf("USB reset took %d loops\n", count);
        delay(25);
    }
    
    USBPHY1_CTRL_CLR = USBPHY_CTRL_CLKGATE;
    USBPHY1_PWD = 0;
    
    USB1_USBMODE = USB_USBMODE_CM(2) | USB_USBMODE_SLOM;
    memset(endpoint_queue_head, 0, sizeof(endpoint_queue_head));
    endpoint_queue_head[0].config = (64 << 16) | (1 << 15);
    endpoint_queue_head[1].config = (64 << 16);
    USB1_ENDPOINTLISTADDR = (uint32_t)&endpoint_queue_head;
    
    USB1_USBINTR = USB_USBINTR_UE | USB_USBINTR_UEE | 
        USB_USBINTR_URE | USB_USBINTR_SLE;
    
    attachInterruptVector(IRQ_USB1, &usb_isr);
    NVIC_ENABLE_IRQ(IRQ_USB1);
    
    USB1_USBCMD = USB_USBCMD_RS;
}

FLASHMEM __attribute__((noinline)) void _reboot_Teensyduino_(void)
{
    if (!(HW_OCOTP_CFG5 & 0x02)) {
        asm("bkpt #251"); // run bootloader
    } else {
        __disable_irq();
        USB1_USBCMD = 0;
        IOMUXC_GPR_GPR16 = 0x00200003;
        __asm__ volatile("mov sp, %0" : : "r" (0x20201000) : );
        __asm__ volatile("dsb":::"memory");
        volatile uint32_t * const p = (uint32_t *)0x20208000;
        *p = 0xEB120000;
        ((void (*)(volatile void *))(*(uint32_t *)(*(uint32_t *)0x0020001C + 8)))(p);
    }
    __builtin_unreachable();
}

void usb_isr(void)
{
    uint32_t status = USB1_USBSTS;
    USB1_USBSTS = status;
    
    // Check device connection periodically
    check_device_connection();
    
    if (status & USB_USBSTS_UI) {
        uint32_t setupstatus = USB1_ENDPTSETUPSTAT;
        while (setupstatus) {
            USB1_ENDPTSETUPSTAT = setupstatus;
            setup_t s;
            do {
                USB1_USBCMD |= USB_USBCMD_SUTW;
                s.word1 = endpoint_queue_head[0].setup0;
                s.word2 = endpoint_queue_head[0].setup1;
            } while (!(USB1_USBCMD & USB_USBCMD_SUTW));
            USB1_USBCMD &= ~USB_USBCMD_SUTW;
            USB1_ENDPTFLUSH = (1<<16) | (1<<0);
            while (USB1_ENDPTFLUSH & ((1<<16) | (1<<0))) ;
            endpoint0_notify_mask = 0;
            endpoint0_setup(s.bothwords);
            setupstatus = USB1_ENDPTSETUPSTAT;
        }
        
        uint32_t completestatus = USB1_ENDPTCOMPLETE;
        if (completestatus) {
            USB1_ENDPTCOMPLETE = completestatus;
            if (completestatus & endpoint0_notify_mask) {
                endpoint0_notify_mask = 0;
                endpoint0_complete();
            }
            completestatus &= endpointN_notify_mask;
            if (completestatus) {
                uint32_t tx = completestatus >> 16;
                while (tx) {
                    int p=__builtin_ctz(tx);
                    run_callbacks(endpoint_queue_head + p * 2 + 1);
                    tx &= ~(1<<p);
                }
                uint32_t rx = completestatus & 0xffff;
                while(rx) {
                    int p=__builtin_ctz(rx);
                    run_callbacks(endpoint_queue_head + p * 2);
                    rx &= ~(1<<p);
                };
            }
        }
    }
    
    if (status & USB_USBSTS_URI) {
        USB1_ENDPTSETUPSTAT = USB1_ENDPTSETUPSTAT;
        USB1_ENDPTCOMPLETE = USB1_ENDPTCOMPLETE;
        while (USB1_ENDPTPRIME != 0) ;
        USB1_ENDPTFLUSH = 0xFFFFFFFF;
        #if defined(CDC_STATUS_INTERFACE) && defined(CDC_DATA_INTERFACE)
        usb_serial_reset();
        #endif
        endpointN_notify_mask = 0;
        proxy_enumeration_complete = false;
    }
    
    if (status & USB_USBSTS_TI0) {
        if (usb_timer0_callback != NULL) usb_timer0_callback();
    }
    if (status & USB_USBSTS_TI1) {
        if (usb_timer1_callback != NULL) usb_timer1_callback();
    }
    if (status & USB_USBSTS_PCI) {
        if (USB1_PORTSC1 & USB_PORTSC1_HSP) {
            usb_high_speed = 1;
        } else {
            usb_high_speed = 0;
        }
    }
    if (status & USB_USBSTS_SLI) {
        // suspend
    }
    if ((USB1_USBINTR & USB_USBINTR_SRE) && (status & USB_USBSTS_SRI)) {
        if (usb_reboot_timer) {
            if (--usb_reboot_timer == 0) {
                usb_stop_sof_interrupts(NUM_INTERFACE);
                _reboot_Teensyduino_();
            }
        }
        #ifdef MIDI_INTERFACE
        usb_midi_flush_output();
        #endif
        #ifdef MULTITOUCH_INTERFACE
        usb_touchscreen_update_callback();
        #endif
        #ifdef FLIGHTSIM_INTERFACE
        usb_flightsim_flush_output();
        #endif
    }
}

static void endpoint0_setup(uint64_t setupdata)
{
    setup_t setup;
    
    setup.bothwords = setupdata;
    
    printf("USB Proxy: Setup %08lX %08lX\n", setup.word1, setup.word2);
    
    // Check if we have a connected device to proxy
    if (!proxy_device_connected || !usb_host_is_ready()) {
        // No device connected, stall
        USB1_ENDPTCTRL0 = 0x000010001;
        return;
    }
    
    // Handle standard requests that might need special handling
    switch (setup.wRequestAndType) {
    case 0x0500: // SET_ADDRESS
        // Let the host handle this locally
        endpoint0_receive(NULL, 0, 0);
        USB1_DEVICEADDR = USB_DEVICEADDR_USBADR(setup.wValue) | USB_DEVICEADDR_USBADRA;
        return;
        
    case 0x0900: // SET_CONFIGURATION
        // Forward to device but also configure our endpoints
        {
            uint16_t dummy_len;
            if (forward_control_request(&setup, NULL, &dummy_len)) {
                usb_configuration = setup.wValue;
                proxy_config_value = setup.wValue;
                
                // Parse config descriptor to set up endpoints
                if (config_descriptor_length > 0) {
                    uint8_t *p = config_descriptor_cache;
                    uint8_t *end = p + config_descriptor_length;
                    
                    num_proxy_endpoints = 0;
                    
                    while (p < end) {
                        uint8_t desc_len = p[0];
                        uint8_t desc_type = p[1];
                        
                        if (p + desc_len > end) break;
                        
                        if (desc_type == 0x05 && desc_len >= 7) { // Endpoint descriptor
                            uint8_t ep_addr = p[2];
                            uint8_t ep_attr = p[3];
                            uint16_t max_packet = p[4] | (p[5] << 8);
                            uint8_t interval = p[6];
                            
                            uint8_t ep_num = ep_addr & 0x0F;
                            uint8_t ep_dir = (ep_addr & 0x80) ? 1 : 0;
                            uint8_t ep_type = ep_attr & 0x03;
                            
                            if (ep_num > 0 && ep_num <= 7 && num_proxy_endpoints < 8) {
                                endpoint_proxy_t *ep = &proxy_endpoints[num_proxy_endpoints++];
                                ep->ep_num = ep_num;
                                ep->ep_dir = ep_dir;
                                ep->max_packet_size = max_packet;
                                ep->ep_type = ep_type;
                                ep->interval = interval;
                                ep->active_buffer = 0;
                                ep->busy[0] = false;
                                ep->busy[1] = false;
                                
                                // Configure endpoint
                                // The ENDPTCTRL register format is:
                                // TX bits [23:16]: TXE=bit23, TXR=bit22, TXT=bits[19:18]
                                // RX bits [7:0]: RXE=bit7, RXR=bit6, RXT=bits[3:2]
                                uint32_t current_ctrl = *((volatile uint32_t *)&USB1_ENDPTCTRL0 + ep_num);
                                
                                if (ep_dir) {
                                    // TX (IN) endpoint
                                    current_ctrl &= 0x0000FFFF; // Clear TX bits
                                    current_ctrl |= (1 << 23);  // TXE - TX Enable
                                    current_ctrl |= (1 << 22);  // TXR - TX Data Toggle Reset
                                    current_ctrl |= ((uint32_t)ep_type << 18); // TXT - TX Endpoint Type
                                } else {
                                    // RX (OUT) endpoint  
                                    current_ctrl &= 0xFFFF0000; // Clear RX bits
                                    current_ctrl |= (1 << 7);   // RXE - RX Enable
                                    current_ctrl |= (1 << 6);   // RXR - RX Data Toggle Reset
                                    current_ctrl |= ((uint32_t)ep_type << 2);  // RXT - RX Endpoint Type
                                }
                                
                                *((volatile uint32_t *)&USB1_ENDPTCTRL0 + ep_num) = current_ctrl;
                                
                                // Set up endpoint queue head
                                endpoint_queue_head[ep_num * 2 + ep_dir].config = 
                                    (max_packet << 16) | ((ep_type == 0) ? (1 << 15) : 0);
                                endpoint_queue_head[ep_num * 2 + ep_dir].callback_function = 
                                    ep_dir ? proxy_in_complete : proxy_out_complete;
                                
                                // Enable notifications
                                if (ep_dir) {
                                    endpointN_notify_mask |= (1 << (ep_num + 16));
                                } else {
                                    endpointN_notify_mask |= (1 << ep_num);
                                }
                                
                                printf("USB Proxy: Configured EP%d%s type=%d size=%d\n",
                                    ep_num, ep_dir ? "IN" : "OUT", ep_type, max_packet);
                            }
                        }
                        
                        p += desc_len;
                    }
                }
                
                endpoint0_receive(NULL, 0, 0);
                proxy_enumeration_complete = true;
                return;
            }
        }
        break;
        
    case 0x0880: // GET_CONFIGURATION
        reply_buffer[0] = usb_configuration;
        endpoint0_transmit(reply_buffer, 1, 0);
        return;
        
    case 0x0680: // GET_DESCRIPTOR
    case 0x0681:
        {
            uint8_t desc_type = setup.wValue >> 8;
            uint8_t desc_index = setup.wValue & 0xFF;
            
            printf("USB Proxy: GET_DESCRIPTOR type=%d index=%d\n", desc_type, desc_index);
            
            if (desc_type == 1) { // Device descriptor
                if (device_descriptor_cache[0] != 0) {
                    uint32_t len = 18;
                    if (len > setup.wLength) len = setup.wLength;
                    endpoint0_transmit(device_descriptor_cache, len, 0);
                    return;
                }
            }
            else if (desc_type == 2) { // Configuration descriptor
                // First request is usually for 9 bytes to get total length
                if (setup.wLength == 9 || config_descriptor_length == 0) {
                    // Need to fetch from device
                    uint16_t actual_len = 0;
                    if (forward_control_request(&setup, config_descriptor_cache, &actual_len)) {
                        config_descriptor_length = actual_len;
                        uint32_t len = actual_len;
                        if (len > setup.wLength) len = setup.wLength;
                        endpoint0_transmit(config_descriptor_cache, len, 0);
                        return;
                    }
                } else {
                    // Use cached descriptor
                    uint32_t len = config_descriptor_length;
                    if (len > setup.wLength) len = setup.wLength;
                    endpoint0_transmit(config_descriptor_cache, len, 0);
                    return;
                }
            }
            else if (desc_type == 3) { // String descriptor
                uint16_t actual_len = 0;
                if (forward_control_request(&setup, string_descriptor_cache, &actual_len)) {
                    uint32_t len = actual_len;
                    if (len > setup.wLength) len = setup.wLength;
                    endpoint0_transmit(string_descriptor_cache, len, 0);
                    return;
                }
            }
        }
        break;
        
    case 0x0080: // GET_STATUS (device)
    case 0x0082: // GET_STATUS (endpoint)
    case 0x0302: // SET_FEATURE (endpoint)
    case 0x0102: // CLEAR_FEATURE (endpoint)
        // Forward these to the real device
        break;
    }
    
    // For all other requests, forward to the device
    uint16_t response_len = 0;
    pending_setup = setup;
    
    if (setup.bmRequestType & 0x80) {
        // Device-to-host (IN) transfer
        if (forward_control_request(&setup, control_proxy_buffer, &response_len)) {
            if (response_len > 0) {
                uint32_t len = response_len;
                if (len > setup.wLength) len = setup.wLength;
                endpoint0_transmit(control_proxy_buffer, len, 0);
            } else {
                endpoint0_receive(NULL, 0, 0);
            }
        } else {
            // Request failed, stall
            USB1_ENDPTCTRL0 = 0x000010001;
        }
    } else {
        // Host-to-device (OUT) transfer
        if (setup.wLength > 0) {
            // Need to receive data first
            endpoint0_receive(endpoint0_buffer, setup.wLength, 1);
        } else {
            // No data phase
            if (forward_control_request(&setup, NULL, &response_len)) {
                endpoint0_receive(NULL, 0, 0);
            } else {
                USB1_ENDPTCTRL0 = 0x000010001;
            }
        }
    }
}

static void endpoint0_transmit(const void *data, uint32_t len, int notify)
{
    if (len > 0) {
        endpoint0_transfer_data.next = 1;
        endpoint0_transfer_data.status = (len << 16) | (1<<7);
        uint32_t addr = (uint32_t)data;
        endpoint0_transfer_data.pointer0 = addr;
        endpoint0_transfer_data.pointer1 = addr + 4096;
        endpoint0_transfer_data.pointer2 = addr + 8192;
        endpoint0_transfer_data.pointer3 = addr + 12288;
        endpoint0_transfer_data.pointer4 = addr + 16384;
        endpoint_queue_head[1].next = (uint32_t)&endpoint0_transfer_data;
        endpoint_queue_head[1].status = 0;
        USB1_ENDPTPRIME |= (1<<16);
        while (USB1_ENDPTPRIME) ;
    }
    endpoint0_transfer_ack.next = 1;
    endpoint0_transfer_ack.status = (1<<7) | (notify ? (1 << 15) : 0);
    endpoint0_transfer_ack.pointer0 = 0;
    endpoint_queue_head[0].next = (uint32_t)&endpoint0_transfer_ack;
    endpoint_queue_head[0].status = 0;
    USB1_ENDPTCOMPLETE = (1<<0) | (1<<16);
    USB1_ENDPTPRIME |= (1<<0);
    endpoint0_notify_mask = (notify ? (1 << 0) : 0);
    while (USB1_ENDPTPRIME) ;
}

static void endpoint0_receive(void *data, uint32_t len, int notify)
{
    if (len > 0) {
        endpoint0_transfer_data.next = 1;
        endpoint0_transfer_data.status = (len << 16) | (1<<7);
        uint32_t addr = (uint32_t)data;
        endpoint0_transfer_data.pointer0 = addr;
        endpoint0_transfer_data.pointer1 = addr + 4096;
        endpoint0_transfer_data.pointer2 = addr + 8192;
        endpoint0_transfer_data.pointer3 = addr + 12288;
        endpoint0_transfer_data.pointer4 = addr + 16384;
        endpoint_queue_head[0].next = (uint32_t)&endpoint0_transfer_data;
        endpoint_queue_head[0].status = 0;
        USB1_ENDPTPRIME |= (1<<0);
        while (USB1_ENDPTPRIME) ;
    }
    endpoint0_transfer_ack.next = 1;
    endpoint0_transfer_ack.status = (1<<7) | (notify ? (1 << 15) : 0);
    endpoint0_transfer_ack.pointer0 = 0;
    endpoint_queue_head[1].next = (uint32_t)&endpoint0_transfer_ack;
    endpoint_queue_head[1].status = 0;
    USB1_ENDPTCOMPLETE = (1<<0) | (1<<16);
    USB1_ENDPTPRIME |= (1<<16);
    endpoint0_notify_mask = (notify ? (1 << 16) : 0);
    while (USB1_ENDPTPRIME) ;
}

static void endpoint0_complete(void)
{
    // Handle completion of control transfers with data phase
    if (pending_setup.wLength > 0 && !(pending_setup.bmRequestType & 0x80)) {
        // This was a host-to-device transfer with data
        uint16_t dummy_len;
        forward_control_request(&pending_setup, endpoint0_buffer, &dummy_len);
    }
}

static bool forward_control_request(setup_t *setup, uint8_t *data, uint16_t *len)
{
    if (!usb_host_is_ready()) {
        return false;
    }
    
    // Pause data transfers during control transfer
    usb_host_pause_transfers();
    
    bool success = usb_host_control_transfer(
        setup->bmRequestType,
        setup->bRequest,
        setup->wValue,
        setup->wIndex,
        setup->wLength,
        data,
        len,
        500  // 500ms timeout
    );
    
    // Resume data transfers
    usb_host_resume_transfers();
    
    return success;
}

static void proxy_in_complete(transfer_t *transfer)
{
    // Find which endpoint this is for
    uint32_t ep_num = 0;
    for (int i = 0; i < num_proxy_endpoints; i++) {
        endpoint_proxy_t *ep = &proxy_endpoints[i];
        if (ep->ep_dir == 1 && 
            (transfer == ep->active_transfers[0] || transfer == ep->active_transfers[1])) {
            ep_num = ep->ep_num;
            
            // Mark buffer as not busy
            int buf_idx = (transfer == ep->active_transfers[0]) ? 0 : 1;
            ep->busy[buf_idx] = false;
            
            // TODO: Here you can intercept mouse data if needed
            // if (ep_num == 1) { // If this is the mouse endpoint
            //     // Modify data in ep->buffer[buf_idx] before sending
            // }
            
            // Forward data to host
            // The transfer structure doesn't have a length field
            // We need to calculate it from the status
            uint32_t remaining = (transfer->status >> 16) & 0x7FFF;
            uint32_t len = ep->max_packet_size - remaining;
            
            if (len > 0) {
                // Queue transfer to host
                usb_prepare_transfer(transfer, ep->buffer[buf_idx], len, 0);
                usb_transmit(ep_num, transfer);
            }
            
            break;
        }
    }
}

static void proxy_out_complete(transfer_t *transfer)
{
    // Find which endpoint this is for and forward to device
    // This is less common for HID devices but needed for completeness
    uint32_t remaining = (transfer->status >> 16) & 0x7FFF;
    // Calculate actual received length from max packet size
    // Note: This would need the endpoint info to get max_packet_size
    if (remaining < 512 && usb_host_is_ready()) { // Assuming max 512 bytes
        // TODO: Implement OUT endpoint forwarding if needed
    }
}

static void check_device_connection(void)
{
    static uint32_t last_check = 0;
    uint32_t now = millis();
    
    if (now - last_check < 100) return;  // Check every 100ms
    last_check = now;
    
    bool currently_connected = usb_host_is_ready();
    
    if (proxy_device_connected && !currently_connected) {
        // Device was connected but now disconnected
        printf("USB Proxy: Device disconnected, rebooting...\n");
        delay(100);  // Give time for message to print
        _reboot_Teensyduino_();
    }
    else if (!proxy_device_connected && currently_connected) {
        // New device connected
        proxy_device_connected = true;
        printf("USB Proxy: New device connected\n");
    }
}

static void run_callbacks(endpoint_t *ep)
{
    transfer_t *first = ep->first_transfer;
    if (first == NULL) return;

    uint32_t count = 0;
    transfer_t *t = first;
    while (1) {
        if (t->status & (1<<7)) {
            ep->first_transfer = t;
            break;
        }
        count++;
        t = (transfer_t *)t->next;
        if ((uint32_t)t == 1) {
            ep->first_transfer = NULL;
            ep->last_transfer = NULL;
            break;
        }
    }
    while (count) {
        transfer_t *next = (transfer_t *)first->next;
        ep->callback_function(first);
        first = next;
        count--;
    }
}

void usb_start_sof_interrupts(int interface)
{
    __disable_irq();
    sof_usage |= (1 << interface);
    uint32_t intr = USB1_USBINTR;
    if (!(intr & USB_USBINTR_SRE)) {
        USB1_USBSTS = USB_USBSTS_SRI;
        USB1_USBINTR = intr | USB_USBINTR_SRE;
    }
    __enable_irq();
}

void usb_stop_sof_interrupts(int interface)
{
    sof_usage &= ~(1 << interface);
    if (sof_usage == 0) {
        USB1_USBINTR &= ~USB_USBINTR_SRE;
    }
}

void usb_prepare_transfer(transfer_t *transfer, const void *data, uint32_t len, uint32_t param)
{
    transfer->next = 1;
    transfer->status = (len << 16) | (1<<7);
    uint32_t addr = (uint32_t)data;
    transfer->pointer0 = addr;
    transfer->pointer1 = addr + 4096;
    transfer->pointer2 = addr + 8192;
    transfer->pointer3 = addr + 12288;
    transfer->pointer4 = addr + 16384;
    transfer->callback_param = param;
}

static void schedule_transfer(endpoint_t *endpoint, uint32_t epmask, transfer_t *transfer)
{
    if (endpoint->callback_function) {
        transfer->status |= (1<<15);
    }
    __disable_irq();
    transfer_t *last = endpoint->last_transfer;
    if (last) {
        last->next = (uint32_t)transfer;
        if (USB1_ENDPTPRIME & epmask) goto end;
        uint32_t status, cyccnt=ARM_DWT_CYCCNT;
        do {
            USB1_USBCMD |= USB_USBCMD_ATDTW;
            status = USB1_ENDPTSTATUS;
        } while (!(USB1_USBCMD & USB_USBCMD_ATDTW) && (ARM_DWT_CYCCNT - cyccnt < 2400));
        if (status & epmask) goto end;
        endpoint->next = (uint32_t)transfer;
        endpoint->status = 0;
        USB1_ENDPTPRIME |= epmask;
        goto end;
    }
    endpoint->next = (uint32_t)transfer;
    endpoint->status = 0;
    USB1_ENDPTPRIME |= epmask;
    endpoint->first_transfer = transfer;
end:
    endpoint->last_transfer = transfer;
    __enable_irq();
}

void usb_transmit(int endpoint_number, transfer_t *transfer)
{
    if (endpoint_number < 2 || endpoint_number > NUM_ENDPOINTS) return;
    endpoint_t *endpoint = endpoint_queue_head + endpoint_number * 2 + 1;
    uint32_t mask = 1 << (endpoint_number + 16);
    schedule_transfer(endpoint, mask, transfer);
}

void usb_receive(int endpoint_number, transfer_t *transfer)
{
    if (endpoint_number < 2 || endpoint_number > NUM_ENDPOINTS) return;
    endpoint_t *endpoint = endpoint_queue_head + endpoint_number * 2;
    uint32_t mask = 1 << endpoint_number;
    schedule_transfer(endpoint, mask, transfer);
}

uint32_t usb_transfer_status(const transfer_t *transfer)
{
    return transfer->status;
}

// Config functions - minimal implementation for proxy
void usb_config_rx(uint32_t ep, uint32_t packet_size, int do_zlp, void (*cb)(transfer_t *)) {}
void usb_config_tx(uint32_t ep, uint32_t packet_size, int do_zlp, void (*cb)(transfer_t *)) {}
void usb_config_rx_iso(uint32_t ep, uint32_t packet_size, int mult, void (*cb)(transfer_t *)) {}
void usb_config_tx_iso(uint32_t ep, uint32_t packet_size, int mult, void (*cb)(transfer_t *)) {}

#else // defined(NUM_ENDPOINTS)

void usb_init(void) {}

#endif // defined(NUM_ENDPOINTS)