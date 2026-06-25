/**
 * Heap Tracing — before/after instrumentation for leak hunting.
 *
 * Lightweight helpers to capture free heap + largest contiguous block around
 * suspicious operations (BLE connect/disconnect, WiFi reconnect, ...) and print
 * the delta to Serial. All output is gated behind `heapDebugEnabled` ("debug
 * heap on"), so it costs nothing in normal operation.
 *
 * Reading the output:
 *   - free drops AND largest drops together over many cycles  → real leak
 *   - free stays ~flat but largest keeps shrinking            → fragmentation
 *   - a single cycle nets clearly negative and never recovers → that cycle leaks
 */

#ifndef HEAP_TRACE_H
#define HEAP_TRACE_H

#include <Arduino.h>
#include "../config.h"  // heapDebugEnabled

struct HeapSnapshot {
    size_t freeHeap;
    size_t largestBlock;
};

// Capture current heap state. Cheap; safe to call unconditionally.
inline HeapSnapshot heapSnapshot() {
    return HeapSnapshot{ ESP.getFreeHeap(), ESP.getMaxAllocHeap() };
}

// Log absolute heap at a named checkpoint (no delta).
inline void heapTraceMark(const char* label) {
    if (!heapDebugEnabled) return;
    HeapSnapshot s = heapSnapshot();
    Serial.printf("HEAPTRACE %-24s free=%6u  largest=%6u\n",
                  label, (unsigned)s.freeHeap, (unsigned)s.largestBlock);
}

// Log the delta between an earlier snapshot and now. Negative deltas mean the
// operation consumed heap that was not given back.
inline void heapTraceDelta(const char* label, const HeapSnapshot& before) {
    if (!heapDebugEnabled) return;
    HeapSnapshot after = heapSnapshot();
    long dFree  = (long)after.freeHeap     - (long)before.freeHeap;
    long dBlock = (long)after.largestBlock - (long)before.largestBlock;
    Serial.printf("HEAPTRACE %-24s free=%6u (%+5ld)  largest=%6u (%+5ld)\n",
                  label, (unsigned)after.freeHeap, dFree,
                  (unsigned)after.largestBlock, dBlock);
}

#endif // HEAP_TRACE_H
