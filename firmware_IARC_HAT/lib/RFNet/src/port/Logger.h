#pragma once

/*
 * Platform-agnostic debug logger for embedded systems.
 *
 * Auto-detected backends (in priority order):
 *   Zephyr OS   - printk()
 *   nRF52 + RTT - SEGGER_RTT       (define USE_SEGGER_RTT)
 *   ESP32       - ets_printf()
 *   Arduino     - Print* (Serial)
 *   Baremetal / FreeRTOS - printf()
 *
 * Compile-time options:
 *   LOGGER_ENABLED=0      disable everything
 *   LOGGER_LEVEL=N        0=none 1=error 2=warn 3=info 4=debug (default 4)
 *   LOGGER_BUFFER_SIZE=N  format buffer bytes (default 128)
 *   USE_SEGGER_RTT        force SEGGER RTT backend
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#ifndef LOGGER_ENABLED
#  define LOGGER_ENABLED 0
#endif

#ifndef LOGGER_BUFFER_SIZE
#  define LOGGER_BUFFER_SIZE 128
#endif

#define LOG_LEVEL_NONE  0
#define LOG_LEVEL_ERROR 1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_INFO  3
#define LOG_LEVEL_DEBUG 4

#ifndef LOGGER_LEVEL
#  define LOGGER_LEVEL LOG_LEVEL_DEBUG
#endif

#if defined(CONFIG_ZEPHYR_KERNEL)
#  include <zephyr/sys/printk.h>
#  define _LOGGER_ZEPHYR

#elif defined(USE_SEGGER_RTT) && defined(__has_include) && __has_include(<SEGGER_RTT.h>)
#  include <SEGGER_RTT.h>
#  define _LOGGER_RTT

#elif defined(ESP32)
#  include <rom/ets_sys.h>
#  define _LOGGER_ESP32

#elif defined(ARDUINO)
#  include <Arduino.h>
#  define _LOGGER_ARDUINO

#else
#  include <stdio.h>
#  define _LOGGER_PRINTF
#endif

class Logger {
public:
    using WriteFn = void (*)(const char* str);

    static bool& _enabled() {
        static bool v = true;
        return v;
    }

    static WriteFn& _writeFn() {
        static WriteFn fn = &_platformWrite;
        return fn;
    }

    // Runtime cap on top of the compile-time LOGGER_LEVEL floor.
    static uint8_t& _level() {
        static uint8_t v = LOGGER_LEVEL;
        return v;
    }

    static void setEnabled(bool e)  { _enabled() = e; }
    static bool isEnabled()         { return _enabled(); }

    static void    setLevel(uint8_t lvl) { _level() = lvl; }
    static uint8_t level()               { return _level(); }

    static bool levelEnabled(char c) {
        uint8_t n = (c == 'E') ? LOG_LEVEL_ERROR
                  : (c == 'W') ? LOG_LEVEL_WARN
                  : (c == 'I') ? LOG_LEVEL_INFO
                  :              LOG_LEVEL_DEBUG;
        return n <= _level();
    }

    static void setWriteFn(WriteFn fn) { _writeFn() = fn; }

#if defined(_LOGGER_ARDUINO)
    static void setOutput(Print& out) {
        _print() = &out;
        _writeFn() = &_arduinoWrite;
    }
#endif

private:
    static void _platformWrite(const char* str) {
#if defined(_LOGGER_ZEPHYR)
        printk("%s", str);
#elif defined(_LOGGER_ESP32)
        ets_printf("%s", str);
#elif defined(_LOGGER_RTT)
        SEGGER_RTT_WriteString(0, str);
#elif defined(_LOGGER_ARDUINO)
        if (_print()) _print()->print(str);
#else
        printf("%s", str);
#endif
    }

#if defined(_LOGGER_ARDUINO)
    static Print*& _print() {
        static Print* p = &Serial;
        return p;
    }
    static void _arduinoWrite(const char* str) {
        if (_print()) _print()->print(str);
    }
#endif
};

#if LOGGER_ENABLED

// Prefix and message share one buffer to limit stack use (matters on AVR).
inline void _logLine(char level, const char* module, const char* format, va_list args) {
    char line[LOGGER_BUFFER_SIZE];
    int n = (level != 0)
          ? snprintf(line, sizeof(line), "[%c][%s] ", level, module)
          : snprintf(line, sizeof(line), "[%s] ", module);
    if (n < 0) return;
    size_t off = (size_t)n < sizeof(line) - 1 ? (size_t)n : sizeof(line) - 1;
    vsnprintf(line + off, sizeof(line) - off, format, args);
    // Append newline, truncating if the message filled the buffer.
    size_t len = strlen(line);
    if (len >= sizeof(line) - 1) len = sizeof(line) - 2;
    line[len]     = '\n';
    line[len + 1] = '\0';
    Logger::_writeFn()(line);
}

inline void log(char level, const char* module, const char* format, ...) {
    if (!Logger::isEnabled() || !Logger::levelEnabled(level)) return;
    va_list args;
    va_start(args, format);
    _logLine(level, module, format, args);
    va_end(args);
}

inline void log(const char* module, const char* format, ...) {
    if (!Logger::isEnabled()) return;
    va_list args;
    va_start(args, format);
    _logLine(0, module, format, args);
    va_end(args);
}

#else

inline void log(char, const char*, const char*, ...) {}
inline void log(const char*, const char*, ...)       {}

#endif

#if LOGGER_ENABLED && LOGGER_LEVEL >= LOG_LEVEL_ERROR
#  define LOG_E(mod, fmt, ...) log('E', mod, fmt, ##__VA_ARGS__)
#else
#  define LOG_E(mod, fmt, ...) do {} while (0)
#endif

#if LOGGER_ENABLED && LOGGER_LEVEL >= LOG_LEVEL_WARN
#  define LOG_W(mod, fmt, ...) log('W', mod, fmt, ##__VA_ARGS__)
#else
#  define LOG_W(mod, fmt, ...) do {} while (0)
#endif

#if LOGGER_ENABLED && LOGGER_LEVEL >= LOG_LEVEL_INFO
#  define LOG_I(mod, fmt, ...) log('I', mod, fmt, ##__VA_ARGS__)
#else
#  define LOG_I(mod, fmt, ...) do {} while (0)
#endif

#if LOGGER_ENABLED && LOGGER_LEVEL >= LOG_LEVEL_DEBUG
#  define LOG_D(mod, fmt, ...) log('D', mod, fmt, ##__VA_ARGS__)
#else
#  define LOG_D(mod, fmt, ...) do {} while (0)
#endif
