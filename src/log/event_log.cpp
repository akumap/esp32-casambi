/**
 * Event Log implementation
 */

#include "event_log.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_system.h>
#include <sys/time.h>
#include <time.h>
#include <stdarg.h>

// ----------------------------------------------------------------------------
// Layer 1 — RTC NOINIT RAM. Survives WDT/panic/SW reset, NOT power-off/brownout.
// These keep their values across a reset as long as RTC RAM stays powered.
// ----------------------------------------------------------------------------
RTC_NOINIT_ATTR static uint32_t rtcLogMagic;                 // == RTC_MAGIC when valid
RTC_NOINIT_ATTR static uint16_t rtcLogHead;                  // next write slot
RTC_NOINIT_ATTR static uint16_t rtcLogCount;                 // valid entries (<= capacity)
RTC_NOINIT_ATTR static uint16_t rtcLogPending;               // newest entries not yet persisted to LittleFS
RTC_NOINIT_ATTR static LogEntry rtcLog[LOG_RTC_CAPACITY];

// Layout-versioned magic: mixing the entry size and ring capacity in means a
// firmware update that changes the LogEntry layout invalidates the RTC ring
// instead of flushing stale bytes from the old layout as garbage entries.
static const uint32_t RTC_MAGIC =
    0xCAFE0000u ^ ((uint32_t)sizeof(LogEntry) << 8) ^ LOG_RTC_CAPACITY;

// Unix time that means "NTP has plausibly synced" (2020-01-01 in seconds).
static const time_t TIME_SYNCED_THRESHOLD = 1577836800;

// ----------------------------------------------------------------------------
// Static members
// ----------------------------------------------------------------------------
bool   EventLog::_initialized = false;
uint16_t EventLog::_bootId    = 0;
char   EventLog::_activeFile  = '0';
size_t EventLog::_activeSize  = 0;
volatile uint32_t EventLog::_generation = 0;
SemaphoreHandle_t EventLog::_mutex = nullptr;
TaskHandle_t EventLog::_ownerTask  = nullptr;

const char* EventLog::levelName(uint8_t level) {
    switch (level) {
        case LOG_DEBUG:    return "DEBUG";
        case LOG_INFO:     return "INFO";
        case LOG_WARN:     return "WARN";
        case LOG_ERROR:    return "ERROR";
        case LOG_CRITICAL: return "CRITICAL";
        default:           return "?";
    }
}

const char* EventLog::_activePath() {
    return _activeFile == '0' ? LOG_FILE_A : LOG_FILE_B;
}

const char* EventLog::_inactivePath() {
    return _activeFile == '0' ? LOG_FILE_B : LOG_FILE_A;
}

int64_t EventLog::_nowTimestamp() {
    time_t now = time(nullptr);
    if (now >= TIME_SYNCED_THRESHOLD) {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    }
    // Not yet synced: negative magnitude = uptime in ms.
    return -(int64_t)millis();
}

void EventLog::_readMeta() {
    _activeFile = '0';
    if (LittleFS.exists(LOG_META_FILE)) {
        File f = LittleFS.open(LOG_META_FILE, "r");
        if (f) {
            int c = f.read();
            if (c == '0' || c == '1') _activeFile = (char)c;
            f.close();
        }
    }
    // Determine current size of the active file.
    _activeSize = 0;
    if (LittleFS.exists(_activePath())) {
        File f = LittleFS.open(_activePath(), "r");
        if (f) {
            _activeSize = f.size();
            f.close();
        }
    }
}

void EventLog::_writeMeta() {
    File f = LittleFS.open(LOG_META_FILE, "w");
    if (f) {
        f.write((uint8_t)_activeFile);
        f.close();
    }
}

// Assumes _mutex is held. Returns true if the entry reached LittleFS.
bool EventLog::_appendLittleFS(const LogEntry& e) {
    // Switch to the other file (and clear it) once the active file is full.
    if (_activeSize + sizeof(LogEntry) > LOG_FILE_MAX_SIZE) {
        _activeFile = (_activeFile == '0') ? '1' : '0';
        // Clear the file we are switching into — it holds the oldest generation.
        File clr = LittleFS.open(_activePath(), "w");
        if (clr) clr.close();
        _activeSize = 0;
        _writeMeta();
        _generation++;   // global record indices shifted — invalidate snapshots
    }

    File f = LittleFS.open(_activePath(), "a");
    if (!f) return false;
    bool ok = (f.write((const uint8_t*)&e, sizeof(e)) == sizeof(e));
    if (ok) _activeSize += sizeof(e);
    f.close();
    return ok;
}

void EventLog::begin() {
    if (_initialized) return;

    if (!_mutex) _mutex = xSemaphoreCreateMutex();

    // --- Boot counter (NVS, survives everything incl. power-off) ---
    Preferences prefs;
    if (prefs.begin("eventlog", false)) {
        _bootId = prefs.getUShort("boot", 0) + 1;
        prefs.putUShort("boot", _bootId);
        prefs.end();
    }

    // --- Ensure log directory exists ---
    if (!LittleFS.exists(LOG_DIR)) {
        LittleFS.mkdir(LOG_DIR);
    }

    _readMeta();

    // --- Recover RTC "last words" from the previous boot (if any) ---
    // Only entries that never made it to LittleFS (rtcLogPending) are flushed —
    // log() persists each entry immediately, so on a clean restart nothing is
    // pending and we avoid re-writing (duplicating) the last boot's entries.
    if (rtcLogMagic == RTC_MAGIC && rtcLogPending > 0) {
        uint16_t count = rtcLogPending;
        if (count > rtcLogCount)     count = rtcLogCount;
        if (count > LOG_RTC_CAPACITY) count = LOG_RTC_CAPACITY;
        // Oldest-first iteration so LittleFS keeps chronological order.
        uint16_t start = (rtcLogHead + LOG_RTC_CAPACITY - count) % LOG_RTC_CAPACITY;
        if (xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
            for (uint16_t i = 0; i < count; i++) {
                uint16_t idx = (start + i) % LOG_RTC_CAPACITY;
                _appendLittleFS(rtcLog[idx]);
            }
            xSemaphoreGive(_mutex);
        }
        Serial.printf("EventLog: recovered %u unpersisted RTC entries from previous boot\n", count);
    }

    // --- (Re)initialise RTC ring for this boot ---
    rtcLogMagic   = RTC_MAGIC;
    rtcLogHead    = 0;
    rtcLogCount   = 0;
    rtcLogPending = 0;

    // Only this task (loopTask) persists to LittleFS inline; other tasks leave
    // their entries pending in the RTC ring for flush().
    _ownerTask   = xTaskGetCurrentTaskHandle();
    _initialized = true;

    // --- Record the reset reason as the opening entry of this boot ---
    esp_reset_reason_t reason = esp_reset_reason();
    const char* rstr;
    uint8_t lvl = LOG_INFO;
    switch (reason) {
        case ESP_RST_POWERON:   rstr = "power-on";                 break;
        case ESP_RST_SW:        rstr = "software restart";         break;
        case ESP_RST_DEEPSLEEP: rstr = "deep-sleep wake";          break;
        case ESP_RST_PANIC:     rstr = "PANIC/exception";    lvl = LOG_CRITICAL; break;
        case ESP_RST_INT_WDT:   rstr = "interrupt watchdog"; lvl = LOG_CRITICAL; break;
        case ESP_RST_TASK_WDT:  rstr = "task watchdog";      lvl = LOG_CRITICAL; break;
        case ESP_RST_WDT:       rstr = "other watchdog";     lvl = LOG_CRITICAL; break;
        case ESP_RST_BROWNOUT:  rstr = "brownout";           lvl = LOG_CRITICAL; break;
        case ESP_RST_EXT:       rstr = "external reset";           break;
        default:                rstr = "unknown";                  break;
    }
    log(lvl, "Boot #%u, reset reason: %s (%d)", (unsigned)_bootId, rstr, (int)reason);
}

void EventLog::log(uint8_t level, const char* fmt, ...) {
    if (level > LOG_CRITICAL) level = LOG_CRITICAL;

    LogEntry e;
    e.timestamp_ms = _nowTimestamp();
    e.bootId       = _bootId;
    e.level        = level;

    char buf[LOG_MSG_MAX + 1];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n < 0) n = 0;
    if (n > LOG_MSG_MAX) n = LOG_MSG_MAX;
    e.msgLen = (uint8_t)n;
    memcpy(e.msg, buf, n);
    if (n < LOG_MSG_MAX) memset(e.msg + n, 0, LOG_MSG_MAX - n);

    // Always echo to serial for live visibility.
    Serial.printf("[LOG/%s] %.*s\n", levelName(level), n, buf);

    // Without the mutex we must not touch the shared ring/file state at all:
    // a concurrent writer would corrupt the RTC indices and interleave file
    // appends (torn records). The serial echo above already happened; the
    // entry is lost from the persistent log in this (rare) case.
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        Serial.println("[LOG] dropped (mutex busy)");
        return;
    }

    // Layer 1: RTC RAM (crash-safe). Done even if begin() hasn't run yet —
    // begin() resets the ring, so this is harmless before init.
    if (rtcLogMagic != RTC_MAGIC) {
        rtcLogMagic   = RTC_MAGIC;
        rtcLogHead    = 0;
        rtcLogCount   = 0;
        rtcLogPending = 0;
    }
    rtcLog[rtcLogHead] = e;
    rtcLogHead = (rtcLogHead + 1) % LOG_RTC_CAPACITY;
    if (rtcLogCount < LOG_RTC_CAPACITY) rtcLogCount++;
    // Mark this entry as not-yet-persisted; cleared once LittleFS confirms it.
    if (rtcLogPending < LOG_RTC_CAPACITY) rtcLogPending++;

    // Layer 2: LittleFS — but only inline from the owner task (loopTask).
    // Other tasks (NimBLE host, async_tcp) must not block on a flash write
    // (LittleFS GC can take >100 ms and stalls the flash cache for both
    // cores); their entries stay pending until flush() runs in loop(). A
    // crash before that is covered by the RTC recovery in begin().
    if (_initialized && xTaskGetCurrentTaskHandle() == _ownerTask) {
        _flushPendingLocked();
    }

    if (_mutex) xSemaphoreGive(_mutex);
}

// Persist pending RTC entries (oldest first) to LittleFS. Assumes _mutex held.
void EventLog::_flushPendingLocked() {
    if (!_initialized || rtcLogPending == 0) return;

    uint16_t count = rtcLogPending;
    if (count > rtcLogCount) count = rtcLogCount;
    uint16_t start = (rtcLogHead + LOG_RTC_CAPACITY - count) % LOG_RTC_CAPACITY;
    for (uint16_t i = 0; i < count; i++) {
        if (!_appendLittleFS(rtcLog[(start + i) % LOG_RTC_CAPACITY])) break;
        rtcLogPending--;
    }
}

void EventLog::flush() {
    // Cheap unlocked pre-check: a stale read here only delays the flush by one
    // loop tick; the authoritative check runs again under the mutex.
    if (!_initialized || rtcLogPending == 0) return;

    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    _flushPendingLocked();
    if (_mutex) xSemaphoreGive(_mutex);
}

void EventLog::clear() {
    bool locked = _mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE;

    if (LittleFS.exists(LOG_FILE_A)) LittleFS.remove(LOG_FILE_A);
    if (LittleFS.exists(LOG_FILE_B)) LittleFS.remove(LOG_FILE_B);
    if (LittleFS.exists(LOG_META_FILE)) LittleFS.remove(LOG_META_FILE);
    _activeFile = '0';
    _activeSize = 0;
    _generation++;   // all snapshot indices are void now

    rtcLogMagic   = RTC_MAGIC;
    rtcLogHead    = 0;
    rtcLogCount   = 0;
    rtcLogPending = 0;

    if (locked) xSemaphoreGive(_mutex);
    Serial.println("EventLog: cleared");
}

// JSON-escape a message and write it (without surrounding quotes).
static void writeEscaped(Print& out, const char* s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        switch (c) {
            case '"':  out.print("\\\""); break;
            case '\\': out.print("\\\\"); break;
            case '\n': out.print("\\n");  break;
            case '\r': out.print("\\r");  break;
            case '\t': out.print("\\t");  break;
            default:
                if ((uint8_t)c < 0x20) {
                    char tmp[7];
                    snprintf(tmp, sizeof(tmp), "\\u%04x", (uint8_t)c);
                    out.print(tmp);
                } else {
                    out.print(c);
                }
        }
    }
}

void EventLog::writeEntryJson(Print& out, const LogEntry& e) {
    // For synced entries timestamp_ms is Unix ms (UTC). For pre-sync entries it
    // is -(uptime_ms); formatting that uptime as epoch yields a 1970 date, which
    // is how clients can tell an entry was logged before NTP sync (year 1970,
    // with the time-of-day portion encoding the uptime).
    int64_t ms     = e.timestamp_ms > 0 ? e.timestamp_ms : -e.timestamp_ms;
    time_t  secs   = (time_t)(ms / 1000);
    int     msPart = (int)(ms % 1000);
    struct tm tmv;
    gmtime_r(&secs, &tmv);
    char tsBuf[40];
    size_t len = strftime(tsBuf, sizeof(tsBuf), "%Y-%m-%dT%H:%M:%S", &tmv);
    snprintf(tsBuf + len, sizeof(tsBuf) - len, ".%03dZ", msPart);

    out.print("{\"tsUtc\":\"");
    out.print(tsBuf);
    out.print("\",\"boot\":");
    out.print((unsigned)e.bootId);
    out.print(",\"level\":");
    out.print((unsigned)e.level);
    out.print(",\"levelName\":\"");
    out.print(EventLog::levelName(e.level));
    out.print("\",\"msg\":\"");
    // Clamp: a corrupted/foreign record (mid-switch read, old firmware layout)
    // may carry msgLen > LOG_MSG_MAX; printing that would read past e.msg.
    writeEscaped(out, e.msg, e.msgLen > LOG_MSG_MAX ? LOG_MSG_MAX : e.msgLen);
    out.print("\"}");
}

// Number of fixed-size records in a file (0 if missing). Assumes _mutex held.
size_t EventLog::_fileRecordCount(const char* path) {
    if (!LittleFS.exists(path)) return 0;
    File f = LittleFS.open(path, "r");
    if (!f) return 0;
    size_t c = f.size() / sizeof(LogEntry);
    f.close();
    return c;
}

// Emit records [firstWanted, count) of one file, newest (highest index) first.
// The file is read in small fixed-size batches: peak RAM is BATCH*sizeof(LogEntry)
// on the stack — never a single large heap block. The mutex is held only while a
// batch is read from flash, never during the (potentially slow) output, so other
// tasks can keep logging and no concurrent flash access occurs during printing.
void EventLog::_emitFileReverse(Print& out, const char* path, size_t count,
                                size_t firstWanted, EventLogEmitFn emit, void* ctx) {
    const size_t recSize = sizeof(LogEntry);
    const size_t BATCH = 4;          // 4 * 132 B = 528 B on the stack
    LogEntry buf[BATCH];

    size_t hi = count;               // exclusive upper bound still to emit
    while (hi > firstWanted) {
        size_t lo = (hi - firstWanted > BATCH) ? (hi - BATCH) : firstWanted;
        size_t n  = hi - lo;         // 1..BATCH records, ascending [lo, hi)

        size_t got = 0;
        bool locked = _mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE;
        File f = LittleFS.open(path, "r");
        if (f) {
            f.seek(lo * recSize);
            while (got < n && f.read((uint8_t*)&buf[got], recSize) == (int)recSize) {
                got++;
            }
            f.close();
        }
        if (locked) xSemaphoreGive(_mutex);

        // Output newest-first within the batch (descending), outside the lock.
        for (size_t k = got; k-- > 0; ) emit(out, buf[k], ctx);

        hi = lo;
    }
}

// Emit the newest `maxEntries` records (all if < 0), newest-first, streaming via
// `emit`. Only file sizes are read into RAM up front; records are streamed in
// small batches, so this is safe even when the heap is too fragmented for a
// large contiguous buffer — the failure mode that previously reset the device.
void EventLog::_forEachNewest(Print& out, int maxEntries, EventLogEmitFn emit, void* ctx) {
    // Snapshot the file layout (older = inactive, newer = active) under the lock.
    bool locked = _mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE;
    const char* older = _inactivePath();
    const char* newer = _activePath();
    size_t cOlder = _fileRecordCount(older);
    size_t cNewer = _fileRecordCount(newer);
    if (locked) xSemaphoreGive(_mutex);

    size_t total = cOlder + cNewer;
    if (total == 0) return;

    size_t limit = (maxEntries >= 0 && (size_t)maxEntries < total)
                       ? (size_t)maxEntries : total;
    size_t start = total - limit;    // global index of the oldest wanted entry

    // Newer file holds global indices [cOlder, total). Emit its wanted tail
    // first (newest), then the older file if the range reaches into it.
    size_t firstWantedNewer = (start > cOlder) ? (start - cOlder) : 0;
    _emitFileReverse(out, newer, cNewer, firstWantedNewer, emit, ctx);

    if (start < cOlder) {
        _emitFileReverse(out, older, cOlder, start, emit, ctx);
    }
}

void EventLog::snapshotNewest(int maxEntries, size_t& cOlder, size_t& cNewer,
                              size_t& total, size_t& start) {
    bool locked = _mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE;
    cOlder = _fileRecordCount(_inactivePath());
    cNewer = _fileRecordCount(_activePath());
    if (locked) xSemaphoreGive(_mutex);

    total = cOlder + cNewer;
    size_t limit = (maxEntries >= 0 && (size_t)maxEntries < total)
                       ? (size_t)maxEntries : total;
    start = total - limit;
}

size_t EventLog::readDescRun(size_t from, size_t minGlobal, size_t want,
                             size_t cOlder, size_t cNewer, LogEntry* out) {
    size_t total = cOlder + cNewer;
    if (from >= total || want == 0) return 0;

    // Stay within one file: older = [0, cOlder), newer = [cOlder, total).
    size_t fileBase  = (from >= cOlder) ? cOlder : 0;
    const char* path = (from >= cOlder) ? _activePath() : _inactivePath();
    size_t low = (minGlobal > fileBase) ? minGlobal : fileBase;
    size_t cnt = from - low + 1;
    if (cnt > want) cnt = want;

    const size_t recSize = sizeof(LogEntry);
    size_t lo  = from - cnt + 1;        // ascending start (global index)
    size_t got = 0;

    bool locked = _mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE;
    File f = LittleFS.open(path, "r");
    if (f) {
        f.seek((lo - fileBase) * recSize);
        for (size_t i = 0; i < cnt; i++) {
            if (f.read((uint8_t*)&out[i], recSize) != (int)recSize) break;
            got++;
        }
        f.close();
    }
    if (locked) xSemaphoreGive(_mutex);

    // out[0..got) was read ascending (oldest→newest); reverse to newest-first.
    for (size_t i = 0; i < got / 2; i++) {
        LogEntry tmp = out[i];
        out[i] = out[got - 1 - i];
        out[got - 1 - i] = tmp;
    }
    return got;
}

// Comma tracking for the streamed JSON array (no heap closure needed).
struct JsonEmitCtx { bool first; };
static void emitEntryJson(Print& out, const LogEntry& e, void* ctx) {
    JsonEmitCtx* c = (JsonEmitCtx*)ctx;
    if (!c->first) out.print(',');
    c->first = false;
    EventLog::writeEntryJson(out, e);
}

void EventLog::writeJson(Print& out, int maxEntries) {
    out.print('[');
    JsonEmitCtx ctx = { true };
    _forEachNewest(out, maxEntries, emitEntryJson, &ctx);
    out.print(']');
}

// One aligned, human-readable line per entry (for the serial 'log' command).
static void writeEntryText(Print& out, const LogEntry& e) {
    char ts[40];
    if (e.timestamp_ms > 0) {
        time_t secs = (time_t)(e.timestamp_ms / 1000);
        int    msPart = (int)(e.timestamp_ms % 1000);
        struct tm tmv;
        gmtime_r(&secs, &tmv);
        size_t len = strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
        snprintf(ts + len, sizeof(ts) - len, ".%03dZ", msPart);
    } else {
        // Pre-NTP: magnitude of the timestamp is the uptime in ms.
        unsigned long s = (unsigned long)((-e.timestamp_ms) / 1000);
        int msPart = (int)((-e.timestamp_ms) % 1000);
        snprintf(ts, sizeof(ts), "[up %lu:%02lu:%02lu.%03d]",
                 s / 3600, (s / 60) % 60, s % 60, msPart);
    }

    char prefix[64];
    snprintf(prefix, sizeof(prefix), "%-25s %-8s b%-3u  ",
             ts, EventLog::levelName(e.level), (unsigned)e.bootId);
    out.print(prefix);

    // msg is not necessarily NUL-terminated → print exactly msgLen bytes.
    for (uint8_t i = 0; i < e.msgLen && i < LOG_MSG_MAX; i++) out.print(e.msg[i]);
    out.print('\n');
}

// Row counter so the header / "(no entries)" can be chosen while streaming.
struct TextEmitCtx { size_t count; };
static void emitEntryText(Print& out, const LogEntry& e, void* ctx) {
    ((TextEmitCtx*)ctx)->count++;
    writeEntryText(out, e);
}

void EventLog::writeText(Print& out, int maxEntries) {
    out.println("time (UTC)                level    boot  message");
    out.println("------------------------------------------------------------");
    TextEmitCtx ctx = { 0 };
    _forEachNewest(out, maxEntries, emitEntryText, &ctx);
    if (ctx.count == 0) out.println("(no entries)");
}
