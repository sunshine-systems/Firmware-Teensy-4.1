#ifndef _SUNBOX_LOGGER_H_
#define _SUNBOX_LOGGER_H_

#include <Arduino.h>
#include <stdarg.h>

// Set to 0 to completely strip all logging from the binary
// Set to 1 to enable logging with runtime channel control
#ifndef SUNBOX_LOGGING
#define SUNBOX_LOGGING 1
#endif

// Log channel bitmask values - always available so code can reference
// LOG_BOOT, LOG_CONNECT, etc. regardless of whether logging is enabled.
enum LogChannel : uint8_t {
    LOG_BOOT    = 0x01,
    LOG_CONNECT = 0x02,
    LOG_ENUM    = 0x04,
    LOG_DATA    = 0x08,
    LOG_COMMAND = 0x10,
    LOG_STATS   = 0x20,
    LOG_ERROR   = 0x40,  // Always on, cannot be disabled
};

#if SUNBOX_LOGGING

class SunBoxLogger {
public:
    SunBoxLogger();

    // Initialize with specific stream or use default Serial4
    void begin(Stream* output = nullptr);

    // Simple logging functions
    void error(const char* message);
    void error(const String& message);

    void warning(const char* message);
    void warning(const String& message);

    void startup(const char* message);
    void startup(const String& message);

    void info(const char* message);
    void info(const String& message);

    void debug(const char* message);
    void debug(const String& message);

    void mouse(const char* message);
    void mouse(const String& message);

    // Printf-style formatting
    void errorf(const char* format, ...);
    void warningf(const char* format, ...);
    void startupf(const char* format, ...);
    void infof(const char* format, ...);
    void debugf(const char* format, ...);
    void mousef(const char* format, ...);

    // --- Log channel system ---

    // Channel mask management
    void setChannelMask(uint8_t mask);
    uint8_t getChannelMask();
    void enableChannel(LogChannel ch);
    void disableChannel(LogChannel ch);
    bool isChannelEnabled(LogChannel ch);

    // Channel-gated simple log methods
    inline void debugCh(LogChannel ch, const char* msg) { if (isChannelEnabled(ch)) debug(msg); }
    inline void infoCh(LogChannel ch, const char* msg) { if (isChannelEnabled(ch)) info(msg); }
    inline void warningCh(LogChannel ch, const char* msg) { if (isChannelEnabled(ch)) warning(msg); }
    inline void startupCh(LogChannel ch, const char* msg) { if (isChannelEnabled(ch)) startup(msg); }

private:
    Stream* output_;
    bool initialized_;
    uint8_t channelMask_;

    void printWithPrefix(const char* prefix, const char* message);
    void printWithPrefix(const char* prefix, const String& message);
    void printfWithPrefix(const char* prefix, const char* format, va_list args);
    bool isDebugEnabled() const;

    // Buffer for formatted output
    static const size_t FORMAT_BUFFER_SIZE = 256;
    char formatBuffer_[FORMAT_BUFFER_SIZE];
};

// Global logger instance
extern SunBoxLogger logger;

// --- Channel-gated logging macros ---
// These check the channel BEFORE any string formatting occurs,
// avoiding va_list forwarding issues with the *f() variants.
#define LOG_DEBUG(ch, msg)         do { if (logger.isChannelEnabled(ch)) logger.debug(msg); } while(0)
#define LOG_DEBUGF(ch, fmt, ...)   do { if (logger.isChannelEnabled(ch)) logger.debugf(fmt, ##__VA_ARGS__); } while(0)
#define LOG_INFO(ch, msg)          do { if (logger.isChannelEnabled(ch)) logger.info(msg); } while(0)
#define LOG_INFOF(ch, fmt, ...)    do { if (logger.isChannelEnabled(ch)) logger.infof(fmt, ##__VA_ARGS__); } while(0)
#define LOG_WARNING(ch, msg)       do { if (logger.isChannelEnabled(ch)) logger.warning(msg); } while(0)
#define LOG_WARNINGF(ch, fmt, ...) do { if (logger.isChannelEnabled(ch)) logger.warningf(fmt, ##__VA_ARGS__); } while(0)
#define LOG_STARTUP(ch, msg)       do { if (logger.isChannelEnabled(ch)) logger.startup(msg); } while(0)
#define LOG_STARTUPF(ch, fmt, ...) do { if (logger.isChannelEnabled(ch)) logger.startupf(fmt, ##__VA_ARGS__); } while(0)
#define LOG_MOUSE(ch, msg)         do { if (logger.isChannelEnabled(ch)) logger.mouse(msg); } while(0)
#define LOG_MOUSEF(ch, fmt, ...)   do { if (logger.isChannelEnabled(ch)) logger.mousef(fmt, ##__VA_ARGS__); } while(0)

#else // SUNBOX_LOGGING == 0: Stripped logger - all methods are empty inlines

class SunBoxLogger {
public:
    void begin(Stream* output = nullptr) { (void)output; }

    void error(const char* message) { (void)message; }
    void error(const String& message) { (void)message; }
    void warning(const char* message) { (void)message; }
    void warning(const String& message) { (void)message; }
    void startup(const char* message) { (void)message; }
    void startup(const String& message) { (void)message; }
    void info(const char* message) { (void)message; }
    void info(const String& message) { (void)message; }
    void debug(const char* message) { (void)message; }
    void debug(const String& message) { (void)message; }
    void mouse(const char* message) { (void)message; }
    void mouse(const String& message) { (void)message; }

    void errorf(const char* format, ...) { (void)format; }
    void warningf(const char* format, ...) { (void)format; }
    void startupf(const char* format, ...) { (void)format; }
    void infof(const char* format, ...) { (void)format; }
    void debugf(const char* format, ...) { (void)format; }
    void mousef(const char* format, ...) { (void)format; }

    void setChannelMask(uint8_t mask) { (void)mask; }
    uint8_t getChannelMask() { return 0; }
    void enableChannel(LogChannel ch) { (void)ch; }
    void disableChannel(LogChannel ch) { (void)ch; }
    bool isChannelEnabled(LogChannel ch) { (void)ch; return false; }

    inline void debugCh(LogChannel ch, const char* msg) { (void)ch; (void)msg; }
    inline void infoCh(LogChannel ch, const char* msg) { (void)ch; (void)msg; }
    inline void warningCh(LogChannel ch, const char* msg) { (void)ch; (void)msg; }
    inline void startupCh(LogChannel ch, const char* msg) { (void)ch; (void)msg; }
};

// Global logger instance (still declared so references compile)
extern SunBoxLogger logger;

// All macros as no-ops
#define LOG_DEBUG(ch, msg)         ((void)0)
#define LOG_DEBUGF(ch, fmt, ...)   ((void)0)
#define LOG_INFO(ch, msg)          ((void)0)
#define LOG_INFOF(ch, fmt, ...)    ((void)0)
#define LOG_WARNING(ch, msg)       ((void)0)
#define LOG_WARNINGF(ch, fmt, ...) ((void)0)
#define LOG_STARTUP(ch, msg)       ((void)0)
#define LOG_STARTUPF(ch, fmt, ...) ((void)0)
#define LOG_MOUSE(ch, msg)         ((void)0)
#define LOG_MOUSEF(ch, fmt, ...)   ((void)0)

#endif // SUNBOX_LOGGING

#endif // _SUNBOX_LOGGER_H_
