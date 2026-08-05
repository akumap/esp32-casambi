/**
 * ConsoleRingBuffer: fixed-capacity byte ring buffer for mirrored console
 * output (see console_out.h).
 *
 * Readers track their own position with a monotonically increasing sequence
 * number (total bytes ever written) instead of an index into the buffer, so
 * any number of independent readers can each replay from wherever they last
 * left off — including a client that starts reading long after bytes were
 * written, in which case its cursor is behind oldestAvailable() and read()
 * fast-forwards it, reporting how much was skipped.
 *
 * Arduino-free and NOT thread-safe by itself: callers that write and read
 * from different tasks must serialize access externally. console_out.cpp
 * does this with a critical section, because Console output is produced from
 * the loop, NimBLE host and async_tcp tasks (docs/konzept-tcp-konsole.md,
 * section 4.2).
 */

#ifndef CONSOLE_RING_BUFFER_H
#define CONSOLE_RING_BUFFER_H

#include <cstddef>
#include <cstdint>
#include <cstring>

class ConsoleRingBuffer {
public:
    ConsoleRingBuffer(uint8_t* storage, size_t capacity)
        : _storage(storage), _capacity(capacity), _written(0) {}

    // Appends `len` bytes. Never blocks and never fails: if `len` exceeds the
    // capacity, only the tail (the part that would survive anyway) is kept;
    // otherwise the oldest still-buffered bytes are silently overwritten.
    void write(const uint8_t* data, size_t len) {
        if (len == 0 || _capacity == 0) return;
        if (len > _capacity) {
            data += (len - _capacity);
            len = _capacity;
        }
        size_t pos = (size_t)(_written % _capacity);
        size_t first = _capacity - pos;
        if (first >= len) {
            memcpy(_storage + pos, data, len);
        } else {
            memcpy(_storage + pos, data, first);
            memcpy(_storage, data + first, len - first);
        }
        _written += len;
    }

    // Total bytes ever written — the buffer's current write sequence number.
    uint64_t written() const { return _written; }

    // Oldest sequence number still fully intact for reading.
    uint64_t oldestAvailable() const {
        return _written > _capacity ? _written - _capacity : 0;
    }

    // Copies up to `maxLen` bytes starting at `*cursor` into `dest` and
    // advances `*cursor` by the number of bytes copied. If `*cursor` is
    // older than oldestAvailable() (the reader fell behind and the buffer
    // wrapped past it), it is fast-forwarded to oldestAvailable() first and
    // the number of skipped bytes is ADDED to `*droppedOut` (if non-null) —
    // callers accumulate this across calls rather than it being reset here.
    // Returns the number of bytes actually copied.
    size_t read(uint64_t* cursor, uint8_t* dest, size_t maxLen,
                uint64_t* droppedOut = nullptr) const {
        if (!cursor || maxLen == 0) return 0;
        uint64_t oldest = oldestAvailable();
        if (*cursor < oldest) {
            if (droppedOut) *droppedOut += (oldest - *cursor);
            *cursor = oldest;
        }
        uint64_t avail = _written - *cursor;
        if (avail == 0) return 0;
        size_t n = (avail < (uint64_t)maxLen) ? (size_t)avail : maxLen;
        size_t pos = (size_t)(*cursor % _capacity);
        size_t first = _capacity - pos;
        if (first >= n) {
            memcpy(dest, _storage + pos, n);
        } else {
            memcpy(dest, _storage + pos, first);
            memcpy(dest + first, _storage, n - first);
        }
        *cursor += n;
        return n;
    }

private:
    uint8_t* _storage;
    size_t   _capacity;
    uint64_t _written;
};

#endif // CONSOLE_RING_BUFFER_H
