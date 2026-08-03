/**
 * Serial console: command dispatcher, setup wizard, BLE scan/connect
 *
 * Split out of main.cpp. This is the interactive surface of the firmware —
 * everything a human types on the serial line — kept apart from the boot and
 * loop orchestration that main.cpp is left with.
 *
 * CONCURRENCY: loop-task only, and that matters more here than it looks.
 * Several commands do things no other task may do concurrently: the setup
 * wizard and 'scan' drive the BLE scanner, 'connect' performs a blocking GATT
 * handshake, and 'refresh'/'wifi set' reboot the device. They are safe because
 * the loop task calls handleCommand() synchronously and nothing else here runs
 * in parallel with it. The REST API deliberately does NOT share these paths —
 * it enqueues commands for the loop task instead (webserver.cpp: BleCommand).
 */

#ifndef SERIAL_CONSOLE_H
#define SERIAL_CONSOLE_H

#include <Arduino.h>

// Execute one console command line (already trimmed, non-empty).
void handleCommand(const String& cmd);

// First-run provisioning over serial: scan, pick a network, enter the Casambi
// and WiFi credentials, download the config, reboot.
void runSetupWizard();

// 'scan' / 'connect <n>' — populate scannedDevices and connect to one of them.
void scanForDevices();
void connectToDevice(int index);

#endif // SERIAL_CONSOLE_H
