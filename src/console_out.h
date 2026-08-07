/**
 * Console: single output facade for everything that used to call Serial
 * directly for printing.
 *
 * Every Serial.print/println/printf/write call site in this codebase now
 * goes through Console instead. Serial *input* (begin/available/
 * readStringUntil) is untouched here — this is output-only. Console mirrors
 * everything written into a fixed-size ring buffer (see console_ring_buffer.h)
 * in addition to forwarding to Serial, so the telnet console
 * (docs/concept-tcp-console.md) can replay it to a client without touching
 * any of the ~660 print call sites directly.
 */

#ifndef CONSOLE_OUT_H
#define CONSOLE_OUT_H

#include <Print.h>
#include <cstdint>
#include <cstddef>

class ConsoleOutput : public Print {
public:
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* buffer, size_t size) override;
};

extern ConsoleOutput Console;

// Thread-safe read access to the ring buffer Console mirrors output into.
// Safe to call from any task (see console_out.cpp) — Console.write() itself
// runs on the loop, NimBLE host and async_tcp tasks, so a plain unsynchronised
// read here could tear against a concurrent write.
//
// `*cursor` is a sequence number (see ConsoleRingBuffer), not a buffer index;
// pass 0 to start from the oldest data still available. If `*cursor` fell
// behind and the buffer wrapped past it, it is fast-forwarded and the number
// of skipped bytes is ADDED to `*droppedOut` (accumulate across calls).
size_t consoleRingRead(uint64_t* cursor, uint8_t* dest, size_t maxLen,
                        uint64_t* droppedOut = nullptr);

// Total bytes ever written to the ring buffer (its current sequence number).
uint64_t consoleRingWritten();

// Oldest sequence number still fully intact for reading right now.
uint64_t consoleRingOldestAvailable();

#endif // CONSOLE_OUT_H
