/**
 * BLE auto-reconnect supervisor
 *
 * Split out of main.cpp. Owns the reconnect policy: when to retry, the
 * exponential backoff, the failure budget that ends in a reboot, and the
 * throttled "why is nobody reconnecting" trace.
 *
 * CONCURRENCY
 * -----------
 * The state here is loop-task only, with ONE exception that the API is shaped
 * around: bleNoteLinkLost() is called from the CasambiClient connection-state
 * callback, which runs on the NimBLE host task. It only stores two millis()
 * timestamps and resets the backoff — no allocation, no logging, no BLE calls.
 * Keep it that way; anything heavier belongs in checkAndReconnectBLE(), which
 * the loop task drives.
 *
 * That is also why the counters are plain (non-atomic): a torn read of a
 * millis() timestamp would at worst shift one retry by one interval, and the
 * loop task is the only writer of the failure counter. Do not widen this into
 * a general cross-task API without revisiting that.
 */

#ifndef BLE_RECONNECT_SUPERVISOR_H
#define BLE_RECONNECT_SUPERVISOR_H

#include <Arduino.h>

// Periodic reconnect driver. Cheap to call every loop iteration; rate-limits
// itself to the current backoff interval.
void checkAndReconnectBLE();

// Called from the connection-state callback (NimBLE host task) when the link
// drops for any reason other than a user request: arms an immediate retry and
// resets the backoff. See the concurrency note above.
void bleNoteLinkLost();

// Called after a successful connect (auto-connect at boot, or the manual
// 'connect' command): clears the failure budget and the outage timestamp.
void bleNoteConnected();

// Called when a connect attempt made at boot failed, so the backoff timer
// starts from now rather than firing immediately on the first loop pass.
void bleNoteConnectAttempt();

// --- Status, for 'status' / 'blediag' / 'reconnect' -------------------------

bool          bleReconnectEnabled();
void          setBleReconnectEnabled(bool enabled);   // also clears the budget
uint8_t       bleConsecutiveFailures();
unsigned long bleReconnectBackoffMs();

// millis() when the link was lost, or 0 while it is up / after a manual
// recovery. Used to report how long the gateway has been offline.
unsigned long bleLostAtMs();

#endif // BLE_RECONNECT_SUPERVISOR_H
