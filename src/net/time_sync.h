/**
 * Time synchronisation (SNTP/UTC) and mDNS advertisement
 *
 * Split out of main.cpp. Both concerns live together because they share the
 * same trigger — they are what has to (re)start whenever the device gains an
 * IP address — and the same per-device identity derived from the eFuse MAC.
 *
 * CONCURRENCY: everything here runs on the loop task (setup, the loop's WiFi
 * recovery path, and the serial 'ntp' command). Nothing is called from the
 * async_tcp or BLE tasks, so the state below needs no synchronisation. The
 * web API's NTP change does NOT call in here directly — it hands the value to
 * the loop task via consumeNtpRequest() (see webserver.cpp) precisely so this
 * stays single-task.
 */

#ifndef NET_TIME_SYNC_H
#define NET_TIME_SYNC_H

#include <Arduino.h>

// Short, stable per-device suffix (the LAST two MAC octets), used for the mDNS
// hostname and the setup-AP SSID so several gateways stay distinguishable.
String deviceSuffix();

// Advertise the configured gateway as casambi-XXXX.local so FHEM can find it.
// Idempotent: safe to call from the boot path and from every WiFi/webserver
// recovery path.
void startMDNS();

// Kick off SNTP (UTC). Non-blocking: the first call starts the query, the loop
// re-checks and logs once the clock is valid.
void syncTime();

// Whether NTP has reported a valid wall-clock time yet.
bool timeSynced();

// Set by the loop once the clock becomes valid, and cleared by 'ntp set' /
// POST /api/ntp so the next sync is reported again.
void setTimeSynced(bool synced);

// Candidate server order of the last syncTime() call, for 'ntp status'.
const String& ntpCandidates();

#endif // NET_TIME_SYNC_H
