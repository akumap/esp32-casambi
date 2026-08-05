/**
 * Console: single output facade for everything that used to call Serial
 * directly for printing.
 *
 * Every Serial.print/println/printf/write call site in this codebase now
 * goes through Console instead. Serial *input* (begin/available/
 * readStringUntil) is untouched here — this is output-only. Today Console
 * simply forwards to Serial, so behavior is unchanged; the indirection
 * exists so a future consumer (docs/konzept-tcp-konsole.md: a network
 * console) can be added by extending write() below, without touching the
 * ~660 call sites that currently produce log output.
 */

#ifndef CONSOLE_OUT_H
#define CONSOLE_OUT_H

#include <Print.h>

class ConsoleOutput : public Print {
public:
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* buffer, size_t size) override;
};

extern ConsoleOutput Console;

#endif // CONSOLE_OUT_H
