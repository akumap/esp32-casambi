/**
 * Time synchronisation (SNTP/UTC) and mDNS advertisement — see time_sync.h.
 */

#include "time_sync.h"

#include <WiFi.h>
#include <ESPmDNS.h>
#include "../config.h"
#include "../app_state.h"

// Short, stable per-device suffix (the LAST two MAC octets) used for the mDNS
// hostname and the setup-AP SSID so several gateways stay distinguishable.
// ESP.getEfuseMac() stores MAC octet 0 (the vendor OUI) in the LOWEST byte, so
// `mac & 0xFFFF` would yield the OUI — identical across boards of a batch. The
// device-specific tail lives in bits 32..47. (Keep in sync with the copy in
// setup_portal.cpp.)
String deviceSuffix() {
    uint64_t mac = ESP.getEfuseMac();
    char buf[5];
    sprintf(buf, "%02x%02x",
            (unsigned)((mac >> 32) & 0xFF),    // mac[4]
            (unsigned)((mac >> 40) & 0xFF));   // mac[5], last octet
    return String(buf);
}

// True once MDNS.begin() succeeded, so the recovery paths below can call
// startMDNS() unconditionally without re-registering the responder.
static bool g_mdnsStarted = false;

// Advertise the configured gateway as casambi-XXXX.local so FHEM can find it.
// Idempotent: called from the boot path and from every WiFi/webserver recovery
// path, because a device that boots before the router is up would otherwise
// never become discoverable. ESPmDNS re-announces on later IP changes itself.
void startMDNS() {
    if (g_mdnsStarted) return;
    String host = "casambi-" + deviceSuffix();
    if (MDNS.begin(host.c_str())) {
        g_mdnsStarted = true;
        MDNS.addService("http", "tcp", 80);
        MDNS.addServiceTxt("http", "tcp", "configured", "1");
        MDNS.addServiceTxt("http", "tcp", "build", String(FIRMWARE_BUILD));
        MDNS.addServiceTxt("http", "tcp", "network", networkConfig.networkName);
        Serial.printf("mDNS: http://%s.local/\n", host.c_str());
    } else {
        Serial.println("mDNS: failed to start (will retry on next WiFi recovery)");
    }
}

// Tracks whether NTP has reported a valid wall-clock time yet.
static bool g_timeSynced = false;

bool timeSynced()                 { return g_timeSynced; }
void setTimeSynced(bool synced)   { g_timeSynced = synced; }

// True for RFC 1918 private IPv4 addresses (typical home-LAN router/DNS).
static bool isPrivateIPv4(const IPAddress& ip) {
    uint8_t a = ip[0], b = ip[1];
    return (a == 10) ||
           (a == 172 && b >= 16 && b <= 31) ||
           (a == 192 && b == 168);
}

// Candidate order of the last syncTime() call, for 'ntp status'.
static String g_ntpCandidates;

const String& ntpCandidates() { return g_ntpCandidates; }

// Kick off SNTP. Non-blocking: the first call from setup() starts the query;
// the loop re-checks and logs once time is valid.
//
// Local-first: while the configured server is still the untouched default
// (pool.ntp.org), the DHCP-provided DNS server and the gateway — in home
// networks usually the router, which typically serves NTP — are tried first.
// lwIP-SNTP is built with SNTP_MAX_SERVERS=3 and cycles to the next candidate
// on timeout, so a router without NTP only delays the first sync by a few
// seconds, while a network without internet still gets valid time from the
// router. An explicitly configured server ('ntp set' / POST /api/ntp) always
// takes precedence alone — no auto-detection in that case.
void syncTime() {
    // lwIP's sntp_setservername() stores the passed POINTER, not a copy, so
    // every string handed to configTime() must stay valid for the lifetime of
    // the SNTP session. Static buffers make that unconditional (the heap
    // buffer of networkConfig.ntpServer can move when the setting changes).
    static char cfgServer[65];
    static char dnsServer[16];
    static char gwServer[16];

    String cfg = networkConfig.ntpServer.length() > 0
                     ? networkConfig.ntpServer
                     : String(NTP_SERVER_DEFAULT);
    strlcpy(cfgServer, cfg.c_str(), sizeof(cfgServer));

    const char* localDns = nullptr;
    const char* localGw  = nullptr;
    if (cfg == NTP_SERVER_DEFAULT) {
        IPAddress dns = WiFi.dnsIP();
        IPAddress gw  = WiFi.gatewayIP();
        if (isPrivateIPv4(dns)) {
            strlcpy(dnsServer, dns.toString().c_str(), sizeof(dnsServer));
            localDns = dnsServer;
        }
        if (isPrivateIPv4(gw) && !(gw == dns)) {
            strlcpy(gwServer, gw.toString().c_str(), sizeof(gwServer));
            localGw = gwServer;
        }
    }

    if (localDns && localGw) {
        configTime(0, 0, localDns, localGw, cfgServer);  // UTC (no offset, no DST)
        g_ntpCandidates = String(localDns) + " (DNS), " + localGw + " (gateway), " + cfgServer;
    } else if (localDns) {
        configTime(0, 0, localDns, cfgServer);
        g_ntpCandidates = String(localDns) + " (DNS), " + cfgServer;
    } else if (localGw) {
        configTime(0, 0, localGw, cfgServer);
        g_ntpCandidates = String(localGw) + " (gateway), " + cfgServer;
    } else {
        configTime(0, 0, cfgServer);
        g_ntpCandidates = cfgServer;
    }
    Serial.printf("NTP: time sync requested, server order: %s (UTC)\n",
                  g_ntpCandidates.c_str());
}
