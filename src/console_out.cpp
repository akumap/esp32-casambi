/**
 * Console — see console_out.h.
 */

#include "console_out.h"

#include <Arduino.h>

ConsoleOutput Console;

size_t ConsoleOutput::write(uint8_t c) {
    return Serial.write(c);
}

size_t ConsoleOutput::write(const uint8_t* buffer, size_t size) {
    return Serial.write(buffer, size);
}
