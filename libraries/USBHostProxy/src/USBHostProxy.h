// USBHostProxy.h
#ifndef _USBHOSTPROXY_H_
#define _USBHOSTPROXY_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// External flag that signals when proxy initialization is complete
extern volatile uint8_t readyForProxy;

// C-callable functions
void USBHostProxy_startSequence(void);
uint8_t USBHostProxy_isReady(void);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
// C++ class definition (only visible to C++ code)
class USBHostProxy {
public:
    USBHostProxy();
    void begin();
    void update();
    bool isReady() const;
    
private:
    uint32_t startTime;
    bool sequenceStarted;
    bool ready;
    
    // Add your C++ library objects here
    // Example: SomeLibrary myLib;
};

// Global instance
extern USBHostProxy usbHostProxy;
#endif

#endif // _USBHOSTPROXY_H_