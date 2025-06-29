#ifndef _SUNBOX_LOGGER_H_
#define _SUNBOX_LOGGER_H_

#include <Arduino.h>
#include <stdarg.h>

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
    
private:
    Stream* output_;
    bool initialized_;
    
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

#endif // _SUNBOX_LOGGER_H_