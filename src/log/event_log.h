/**
 * Event Log — non-volatile, time-stamped ring buffer for fatal/diagnostic events
 *
 * Two storage layers:
 *
 *   Layer 1 — RTC NOINIT RAM (~4 KB):
 *     Survives watchdog/panic/SW resets (NOT power-off or brownout). Every
 *     log() writes here first, so the "last words" right before a crash are
 *     captured even if LittleFS never gets the write. On the next boot,
 *     begin() flushes any surviving RTC entries into LittleFS.
 *
 *   Layer 2 — LittleFS ping-pong ring buffer (2 × LOG_FILE_MAX_SIZE):
 *     /log/log_a.bin and /log/log_b.bin. The active file is appended to until
 *     full, then we switch to the other file and clear it. This keeps the most
 *     recent 1–2 files of history while bounding flash usage and wear.
 *
 * Timestamps are Unix milliseconds in UTC once NTP has synced. Before sync,
 * timestamp_ms is stored as a NEGATIVE value whose magnitude is the uptime in
 * ms (millis()); combined with bootId this lets pre-sync entries be ordered
 * and attributed to a specific boot.
 *
 * Usage:
 *   EventLog::log(LOG_ERROR,    "BLE: Auth failed after %d attempts", n);
 *   EventLog::log(LOG_CRITICAL, "WDT reset detected, reason=%d", reason);
 */

#ifndef EVENT_LOG_H
#define EVENT_LOG_H

#include <Arduino.h>
#include "../config.h"

// ----------------------------------------------------------------------------
// Severity levels
// ----------------------------------------------------------------------------
enum LogLevel : uint8_t {
    LOG_DEBUG    = 0,
    LOG_INFO     = 1,
    LOG_WARN     = 2,
    LOG_ERROR    = 3,
    LOG_CRITICAL = 4,
};

// ----------------------------------------------------------------------------
// On-disk / in-RTC record (binary, fixed size). Kept packed so the layout is
// stable across both storage layers. 8 + 2 + 1 + 1 + 120 = 132 bytes.
// ----------------------------------------------------------------------------
struct __attribute__((packed)) LogEntry {
    int64_t  timestamp_ms;        // Unix ms (UTC) if > 0; -(uptime_ms) before NTP sync
    uint16_t bootId;              // boot counter (from NVS); groups pre-sync entries
    uint8_t  level;               // LogLevel
    uint8_t  msgLen;              // valid bytes in msg (<= LOG_MSG_MAX)
    char     msg[LOG_MSG_MAX];    // message text (not necessarily NUL-terminated)
};

// Per-entry output callback used by the streaming readers. `ctx` carries any
// caller state (e.g. JSON comma tracking) so no heap-allocated closure is
// needed. Entries are delivered newest-first.
typedef void (*EventLogEmitFn)(Print& out, const LogEntry& e, void* ctx);

class EventLog {
public:
    /**
     * Initialise the log: mount-dependent setup, boot counter, RTC recovery.
     * MUST be called after ConfigStore::init() (LittleFS must be mounted).
     * Flushes any RTC "last words" from the previous boot into LittleFS and
     * records the reset reason as the first entry of this boot.
     */
    static void begin();

    /**
     * Append a formatted entry. Safe to call from any task (mutex-guarded).
     * Writes to RTC RAM first (crash-safe), then best-effort to LittleFS.
     */
    static void log(uint8_t level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

    /**
     * Erase all persisted entries (both LittleFS files and the RTC buffer).
     */
    static void clear();

    /**
     * Stream the log as a JSON array, newest entry first, to the given Print
     * sink. If maxEntries >= 0, only the newest maxEntries are emitted.
     */
    static void writeJson(Print& out, int maxEntries = -1);

    /**
     * Stream the log as aligned, human-readable text (newest entry first) to
     * the given Print sink — used by the serial 'log' command. If
     * maxEntries >= 0, only the newest maxEntries are emitted.
     */
    static void writeText(Print& out, int maxEntries = -1);

    /**
     * Serialize a single entry as a JSON object (no surrounding array/comma).
     * Public so a chunked HTTP response can stream entries one at a time
     * without buffering the whole array in RAM.
     */
    static void writeEntryJson(Print& out, const LogEntry& e);

    // ------------------------------------------------------------------------
    // Pull-based access for low-memory streaming (e.g. chunked HTTP response).
    // Lets a caller emit the newest entries one at a time, never holding more
    // than a single record in RAM, instead of building the whole array first.
    // ------------------------------------------------------------------------

    /**
     * Snapshot the current record layout. `total` is the number of stored
     * records; `start` is the global index of the oldest record that falls in
     * the newest `maxEntries` window (maxEntries < 0 → all). cOlder/cNewer are
     * the per-file counts needed to map a global index back to a file.
     */
    static void snapshotNewest(int maxEntries, size_t& cOlder, size_t& cNewer,
                               size_t& total, size_t& start);

    /**
     * Read the record at a global index (0 = oldest) using the file counts from
     * snapshotNewest(). Returns false if the index is out of range or the read
     * fails. One small record is read from flash; no large allocation.
     */
    static bool readByGlobal(size_t globalIndex, size_t cOlder, size_t cNewer,
                             LogEntry& out);

    /**
     * Current boot counter (incremented once per begin()).
     */
    static uint16_t bootCount() { return _bootId; }

    /**
     * Human-readable level name (e.g. "ERROR").
     */
    static const char* levelName(uint8_t level);

private:
    static bool   _initialized;
    static uint16_t _bootId;
    static char   _activeFile;     // '0' => A, '1' => B
    static size_t _activeSize;     // bytes currently in active file
    static SemaphoreHandle_t _mutex;

    // Streaming readers — emit the newest `maxEntries` records newest-first
    // without ever holding more than a few records in RAM. The flash is read in
    // small fixed-size batches, so no large contiguous allocation is needed
    // (which would fail on a fragmented heap and reset the device).
    static void   _forEachNewest(Print& out, int maxEntries, EventLogEmitFn emit, void* ctx);
    static void   _emitFileReverse(Print& out, const char* path, size_t count,
                                   size_t firstWanted, EventLogEmitFn emit, void* ctx);
    static size_t _fileRecordCount(const char* path);   // assumes mutex held
    static int64_t _nowTimestamp();                 // signed timestamp per above
    static const char* _activePath();
    static const char* _inactivePath();
    static bool   _appendLittleFS(const LogEntry& e);  // assumes mutex held; true on success
    static void   _writeMeta();
    static void   _readMeta();
};

#endif // EVENT_LOG_H
