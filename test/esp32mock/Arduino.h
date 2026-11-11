#pragma once
#include "esp32mock.h"
#include <stdarg.h>
struct MockSerial { void println(const char*) {} void print(const char*) {} void flush() {} void printf(const char*, ...) {} };
extern MockSerial Serial;
inline void delay(unsigned long) {}
inline void yield() {}
