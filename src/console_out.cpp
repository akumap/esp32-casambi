/**
 * Console — see console_out.h.
 */

#include "console_out.h"

#include <Arduino.h>
#include "config.h"
#include "console_ring_buffer.h"

ConsoleOutput Console;

namespace {

uint8_t g_ringStorage[CONSOLE_RING_BUFFER_SIZE];
ConsoleRingBuffer g_ring(g_ringStorage, sizeof(g_ringStorage));

// A spinlock, not the xSemaphoreTake/Give mutex used elsewhere in this
// codebase (e.g. g_configMutex): Console.write() is called from the NimBLE
// host task, where a full RTOS mutex wait would add latency to BLE-stack
// timing this project already treats as tight (see WDT_TIMEOUT_SECONDS'
// comment on the ~30s ATT procedure timeout). The critical section here only
// ever guards a memcpy of at most a few hundred bytes, so a spinlock is both
// the safer and the standard-for-ESP32-Arduino choice for something this
// short and this hot.
portMUX_TYPE g_ringMux = portMUX_INITIALIZER_UNLOCKED;

}  // namespace

size_t ConsoleOutput::write(uint8_t c) {
    portENTER_CRITICAL(&g_ringMux);
    g_ring.write(&c, 1);
    portEXIT_CRITICAL(&g_ringMux);
    return Serial.write(c);
}

size_t ConsoleOutput::write(const uint8_t* buffer, size_t size) {
    portENTER_CRITICAL(&g_ringMux);
    g_ring.write(buffer, size);
    portEXIT_CRITICAL(&g_ringMux);
    return Serial.write(buffer, size);
}

size_t consoleRingRead(uint64_t* cursor, uint8_t* dest, size_t maxLen, uint64_t* droppedOut) {
    portENTER_CRITICAL(&g_ringMux);
    size_t n = g_ring.read(cursor, dest, maxLen, droppedOut);
    portEXIT_CRITICAL(&g_ringMux);
    return n;
}

uint64_t consoleRingWritten() {
    portENTER_CRITICAL(&g_ringMux);
    uint64_t w = g_ring.written();
    portEXIT_CRITICAL(&g_ringMux);
    return w;
}

uint64_t consoleRingOldestAvailable() {
    portENTER_CRITICAL(&g_ringMux);
    uint64_t o = g_ring.oldestAvailable();
    portEXIT_CRITICAL(&g_ringMux);
    return o;
}
