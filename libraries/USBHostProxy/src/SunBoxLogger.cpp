#include "SunBoxLogger.h"

// Global logger instance
SunBoxLogger logger;

#if SUNBOX_LOGGING

#include "SunBoxStartup.h"

SunBoxLogger::SunBoxLogger() : output_(nullptr), initialized_(false), channelMask_(LOG_ERROR) {
}

void SunBoxLogger::begin(Stream* output) {
    if (output) {
        output_ = output;
    } else {
        // Default to Serial4
        output_ = &Serial4;
    }
    initialized_ = true;
}

void SunBoxLogger::error(const char* message) {
    printWithPrefix("E: ", message);
}

void SunBoxLogger::error(const String& message) {
    printWithPrefix("E: ", message);
}

void SunBoxLogger::warning(const char* message) {
    printWithPrefix("W: ", message);
}

void SunBoxLogger::warning(const String& message) {
    printWithPrefix("W: ", message);
}

void SunBoxLogger::startup(const char* message) {
    printWithPrefix("S: ", message);
}

void SunBoxLogger::startup(const String& message) {
    printWithPrefix("S: ", message);
}

void SunBoxLogger::info(const char* message) {
    printWithPrefix("I: ", message);
}

void SunBoxLogger::info(const String& message) {
    printWithPrefix("I: ", message);
}

void SunBoxLogger::debug(const char* message) {
    if (isDebugEnabled()) {
        printWithPrefix("D: ", message);
    }
}

void SunBoxLogger::debug(const String& message) {
    if (isDebugEnabled()) {
        printWithPrefix("D: ", message);
    }
}

void SunBoxLogger::mouse(const char* message) {
    printWithPrefix("M: ", message);
}

void SunBoxLogger::mouse(const String& message) {
    printWithPrefix("M: ", message);
}

void SunBoxLogger::printWithPrefix(const char* prefix, const char* message) {
    // TODO: Remove this backward compatibility check in future version
    // For now, if not initialized, try to use Serial4 directly if it's available
    if (!initialized_ || !output_) {
        if (Serial4) {
            Serial4.print(prefix);
            Serial4.println(message);
        }
        return;
    }
    output_->print(prefix);
    output_->println(message);
}

void SunBoxLogger::printWithPrefix(const char* prefix, const String& message) {
    // TODO: Remove this backward compatibility check in future version
    // For now, if not initialized, try to use Serial4 directly if it's available
    if (!initialized_ || !output_) {
        if (Serial4) {
            Serial4.print(prefix);
            Serial4.println(message);
        }
        return;
    }
    output_->print(prefix);
    output_->println(message);
}

bool SunBoxLogger::isDebugEnabled() const {
    return SunBoxStartup::isDebugEnabled();
}

// --- Log channel methods ---

void SunBoxLogger::setChannelMask(uint8_t mask) {
    // LOG_ERROR is always on, ensure it cannot be removed
    channelMask_ = mask | LOG_ERROR;
}

uint8_t SunBoxLogger::getChannelMask() {
    return channelMask_;
}

void SunBoxLogger::enableChannel(LogChannel ch) {
    channelMask_ |= ch;
}

void SunBoxLogger::disableChannel(LogChannel ch) {
    // LOG_ERROR cannot be disabled
    if (ch == LOG_ERROR) return;
    channelMask_ &= ~ch;
}

bool SunBoxLogger::isChannelEnabled(LogChannel ch) {
    // LOG_ERROR is always enabled regardless of mask
    if (ch == LOG_ERROR) return true;
    return (channelMask_ & ch) != 0;
}

// Printf-style formatting functions

void SunBoxLogger::errorf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    printfWithPrefix("E: ", format, args);
    va_end(args);
}

void SunBoxLogger::warningf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    printfWithPrefix("W: ", format, args);
    va_end(args);
}

void SunBoxLogger::startupf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    printfWithPrefix("S: ", format, args);
    va_end(args);
}

void SunBoxLogger::infof(const char* format, ...) {
        va_list args;
        va_start(args, format);
        printfWithPrefix("I: ", format, args);
        va_end(args);
}

void SunBoxLogger::debugf(const char* format, ...) {
    if (isDebugEnabled()) {
        va_list args;
        va_start(args, format);
        printfWithPrefix("D: ", format, args);
        va_end(args);
    }
}

void SunBoxLogger::mousef(const char* format, ...) {
    va_list args;
    va_start(args, format);
    printfWithPrefix("M: ", format, args);
    va_end(args);
}

void SunBoxLogger::printfWithPrefix(const char* prefix, const char* format, va_list args) {
    vsnprintf(formatBuffer_, FORMAT_BUFFER_SIZE, format, args);

    // TODO: Remove this backward compatibility check in future version
    // For now, if not initialized, try to use Serial4 directly if it's available
    if (!initialized_ || !output_) {
        if (Serial4) {
            Serial4.print(prefix);
            Serial4.println(formatBuffer_);
        }
        return;
    }
    output_->print(prefix);
    output_->println(formatBuffer_);
}

#endif // SUNBOX_LOGGING
