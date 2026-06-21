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
#include <vector>

// ----------------------------------------------------------------------------
// Layer 1 — RTC NOINIT RAM. Survives WDT/panic/SW reset, NOT power-off/brownout.
// These keep their values across a reset as long as RTC RAM stays powered.
// ----------------------------------------------------------------------------
RTC_NOINIT_ATTR static uint32_t rtcLogMagic;                 // == RTC_MAGIC when valid
RTC_NOINIT_ATTR static uint16_t rtcLogHead;                  // next write slot
RTC_NOINIT_ATTR static uint16_t rtcLogCount;                 // valid entries (<= capacity)
RTC_NOINIT_ATTR static LogEntry rtcLog[LOG_RTC_CAPACITY];

static const uint32_t RTC_MAGIC = 0xCAFEBABE;

// Unix time that means "NTP has plausibly synced" (2020-01-01 in seconds).
static const time_t TIME_SYNCED_THRESHOLD = 1577836800;

// ----------------------------------------------------------------------------
// Static members
// ----------------------------------------------------------------------------
bool   EventLog::_initialized = false;
uint16_t EventLog::_bootId    = 0;
char   EventLog::_activeFile  = '0';
size_t EventLog::_activeSize  = 0;
SemaphoreHandle_t EventLog::_mutex = nullptr;

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

// Assumes _mutex is held.
void EventLog::_appendLittleFS(const LogEntry& e) {
    // Switch to the other file (and clear it) once the active file is full.
    if (_activeSize + sizeof(LogEntry) > LOG_FILE_MAX_SIZE) {
        _activeFile = (_activeFile == '0') ? '1' : '0';
        // Clear the file we are switching into — it holds the oldest generation.
        File clr = LittleFS.open(_activePath(), "w");
        if (clr) clr.close();
        _activeSize = 0;
        _writeMeta();
    }

    File f = LittleFS.open(_activePath(), "a");
    if (!f) return;
    if (f.write((const uint8_t*)&e, sizeof(e)) == sizeof(e)) {
        _activeSize += sizeof(e);
    }
    f.close();
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
    if (rtcLogMagic == RTC_MAGIC && rtcLogCount > 0) {
        uint16_t count = rtcLogCount;
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
        Serial.printf("EventLog: recovered %u RTC entries from previous boot\n", count);
    }

    // --- (Re)initialise RTC ring for this boot ---
    rtcLogMagic = RTC_MAGIC;
    rtcLogHead  = 0;
    rtcLogCount = 0;

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

    bool locked = _mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) == pdTRUE;

    // Layer 1: RTC RAM (crash-safe). Done even if begin() hasn't run yet —
    // begin() resets the ring, so this is harmless before init.
    if (rtcLogMagic != RTC_MAGIC) {
        rtcLogMagic = RTC_MAGIC;
        rtcLogHead  = 0;
        rtcLogCount = 0;
    }
    rtcLog[rtcLogHead] = e;
    rtcLogHead = (rtcLogHead + 1) % LOG_RTC_CAPACITY;
    if (rtcLogCount < LOG_RTC_CAPACITY) rtcLogCount++;

    // Layer 2: LittleFS (best-effort; only once initialised).
    if (_initialized) {
        _appendLittleFS(e);
    }

    if (locked) xSemaphoreGive(_mutex);
}

void EventLog::clear() {
    bool locked = _mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE;

    if (LittleFS.exists(LOG_FILE_A)) LittleFS.remove(LOG_FILE_A);
    if (LittleFS.exists(LOG_FILE_B)) LittleFS.remove(LOG_FILE_B);
    if (LittleFS.exists(LOG_META_FILE)) LittleFS.remove(LOG_META_FILE);
    _activeFile = '0';
    _activeSize = 0;

    rtcLogMagic = RTC_MAGIC;
    rtcLogHead  = 0;
    rtcLogCount = 0;

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

static void writeEntryJson(Print& out, const LogEntry& e) {
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
    writeEscaped(out, e.msg, e.msgLen);
    out.print("\"}");
}

void EventLog::writeJson(Print& out, int maxEntries) {
    bool locked = _mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE;

    const size_t recSize = sizeof(LogEntry);
    const char* paths[2] = { _inactivePath(), _activePath() };  // older, newer

    // Count records per file from their sizes (no allocation).
    size_t counts[2] = { 0, 0 };
    for (int p = 0; p < 2; p++) {
        if (!LittleFS.exists(paths[p])) continue;
        File f = LittleFS.open(paths[p], "r");
        if (!f) continue;
        counts[p] = f.size() / recSize;
        f.close();
    }
    size_t total = counts[0] + counts[1];

    // Only buffer the newest `limit` entries — reading the whole log into RAM
    // can exhaust the heap when WiFi/BLE/web are active (each LogEntry is large).
    size_t limit = (maxEntries >= 0 && (size_t)maxEntries < total)
                       ? (size_t)maxEntries : total;
    size_t start = total - limit;   // global index of first entry to keep

    std::vector<LogEntry> entries;
    entries.reserve(limit);

    size_t globalIdx = 0;
    for (int p = 0; p < 2; p++) {
        size_t fileCount = counts[p];
        if (fileCount == 0) continue;

        // Skip whole file if it lies entirely before the wanted range.
        if (start >= globalIdx + fileCount) { globalIdx += fileCount; continue; }

        File f = LittleFS.open(paths[p], "r");
        if (!f) { globalIdx += fileCount; continue; }

        size_t skipInFile = (start > globalIdx) ? (start - globalIdx) : 0;
        if (skipInFile) f.seek(skipInFile * recSize);

        LogEntry e;
        while (f.read((uint8_t*)&e, recSize) == (int)recSize) {
            entries.push_back(e);
        }
        f.close();
        globalIdx += fileCount;
    }

    if (locked) xSemaphoreGive(_mutex);

    // Emit newest-first.
    out.print('[');
    size_t kept = entries.size();
    bool first = true;
    for (size_t i = 0; i < kept; i++) {
        const LogEntry& e = entries[kept - 1 - i];  // reverse → newest first
        if (!first) out.print(',');
        first = false;
        writeEntryJson(out, e);
    }
    out.print(']');
}
