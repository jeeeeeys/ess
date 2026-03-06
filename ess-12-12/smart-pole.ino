#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266httpUpdate.h>
#include <ESP8266HTTPClient.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include <time.h>
#include <EEPROM.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
extern "C" {
  #include <user_interface.h>
}

#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #error "Missing secrets.h. Copy secrets.example.h to secrets.h and fill values."
#endif

// ================= PIN DEFINITIONS =================
#define PIN_BUZZER     D8
#define PIN_DHT        D7
#define PIN_RELAY_1    D6
#define PIN_RELAY_2    D5
#define PIN_RELAY_3    D4

#if defined(ESP8266)
  #define PMS_SERIAL Serial
  #define DEBUG_SERIAL Serial1
#else
  #define PMS_SERIAL Serial
  #define DEBUG_SERIAL Serial
#endif

// ================= RELAY LOGIC =================
#define RELAY_ON  LOW
#define RELAY_OFF HIGH

// ================= DHT =================
#define DHTTYPE DHT22
DHT dht(PIN_DHT, DHTTYPE);

// ================= PMS =================
enum PmsErrorReason {
  PMS_ERROR_NONE,
  PMS_ERROR_TIMEOUT,
  PMS_ERROR_HEADER,
  PMS_ERROR_LENGTH,
  PMS_ERROR_CHECKSUM
};

PmsErrorReason lastPmsError = PMS_ERROR_NONE;

const unsigned long PMS_READ_TIMEOUT_MS = 300;
const uint16_t PMS_FRAME_LENGTH = 32;
const uint16_t PMS_EXPECTED_DATA_LEN = 0x001C;

// ================= DEVICE INFO =================
#define FW_VERSION "1.0.1"
#define HW_VERSION "revA"
#define OTA_MANIFEST_URL "http://172.40.0.11/smartfactory/api/smart-pole-configuration?category=version"

// ================= WIFI STORAGE =================
#define EEPROM_SIZE 512
#define WIFI_MAX_RETRIES 3
#define MAX_WIFI_NETWORKS 5

struct WiFiCredEntry {
  char ssid[32];
  char pass[64];
  uint8_t valid;
};

struct WiFiStore {
  uint32_t magic;
  uint8_t  lastGoodIndex;
  WiFiCredEntry nets[MAX_WIFI_NETWORKS];
};

static const uint32_t WIFI_STORE_MAGIC = 0x534D5051; // "SMPQ"
WiFiStore wifiStore;

// ================= CLIENTS =================
WiFiClientSecure secureClient;
PubSubClient mqtt(secureClient);
ESP8266WebServer webServer(80);
DNSServer dnsServer;

// ================= WIFI EVENT HANDLERS =================
WiFiEventHandler onStaDisconnected;
WiFiEventHandler onStaGotIP;
volatile bool staDisconnected = false;

// ================= STATE =================
enum SystemState { STATE_IDLE, STATE_RUNNING, STATE_ERROR };
enum LoadLevel  { LOAD_OFF = 0, LOAD_LOW, LOAD_MED, LOAD_HIGH };

SystemState systemState = STATE_RUNNING;
LoadLevel currentLoad   = LOAD_OFF;

// ================= TELEMETRY =================
struct Telemetry {
  uint32_t timestamp;
  bool timeValid;
  uint32_t deviceID;
  LoadLevel load;
  float temperature;
  float humidity;
  uint16_t pm25;
  uint16_t pm10;
  bool sensorValid;
} telemetry;

// ================= GLOBALS =================
bool provisioningActive = false;
uint8_t wifiFailCount = 0;
bool bootOkPublished = false;
bool wifiLostBeeped = false;
bool otaUpdateRequested = false;
unsigned long lastOtaCommandMillis = 0;
const unsigned long OTA_COMMAND_GUARD_MS = 60000UL;
BearSSL::X509List rootCaCert(AWS_ROOT_CA);

// Intervals
const unsigned long SAMPLE_INTERVAL_MS   = 10000;   // 10s
const unsigned long PUBLISH_INTERVAL_MS  = 60000;   // 60s
unsigned long lastSampleMillis   = 0;
unsigned long lastPublishMillis  = 0;

const unsigned long REBOOT_GUARD_WINDOW_MS = 60000UL;
const uint32_t REBOOT_GUARD_MAGIC = 0x52425431; // RBT1
const uint32_t REBOOT_GUARD_RTC_SLOT = 64;

struct RebootGuardRtc {
  uint32_t magic;
  uint32_t cooldownActive;
};

RebootGuardRtc rebootGuardRtc = {0, 0};
bool rebootCooldownActive = false;

// Portal + error periodic beeps
const unsigned long PORTAL_BEEP_INTERVAL_MS = 30000; // 30s
unsigned long lastPortalBeepMillis = 0;

const unsigned long ERROR_BEEP_INTERVAL_MS = 30000;  // 30s
unsigned long lastErrorBeepMillis = 0;

// Sensor failure immediate + periodic
bool sensorFaultActive = false;
unsigned long lastSensorFaultBeepMillis = 0;
const unsigned long SENSOR_FAULT_BEEP_INTERVAL_MS = 30000; // 30s

// Portal background WiFi retry
const unsigned long PORTAL_WIFI_RETRY_INTERVAL_MS = 10000; // 10s
unsigned long lastPortalWiFiRetryMillis = 0;

const unsigned long DHT_WARMUP_MS = 30000; // 30s warmup to avoid false faults
unsigned long dhtWarmupStartMillis = 0;
bool dhtHasValidSample = false;

// Window accumulators
uint8_t dhtSampleCount = 0;
uint8_t dhtValidCount  = 0;
float   sumTemp = 0.0f;
float   sumHum  = 0.0f;

uint8_t pmsSampleCount = 0;
uint8_t pmsValidCount  = 0;
uint32_t sumPM25 = 0;
uint32_t sumPM10 = 0;

const float VALID_RATIO_THRESHOLD = 0.70f;

// ================= TOPICS =================
char topicRegister[64];
char topicData[64];
char topicStatus[64];
char deviceIdHex[7];

// ================= BUZZER =================
static const uint16_t BUZZ_FREQ = 3520;

void buzzerBeep(uint16_t ms = 100) {
  tone(PIN_BUZZER, BUZZ_FREQ);
  delay(ms);
  noTone(PIN_BUZZER);
}

void beepCount(uint8_t n, uint16_t onMs = 80, uint16_t offMs = 120) {
  for (uint8_t i = 0; i < n; i++) {
    buzzerBeep(onMs);
    delay(offMs);
  }
}

void beepLong() {
  buzzerBeep(600);
  delay(200);
}

void beepRapid5() {
  for (uint8_t i = 0; i < 5; i++) {
    buzzerBeep(60);
    delay(60);
  }
  delay(200);
}

// Patterns you requested:
void beepBootOK()        {
  beepCount(1);
}
void beepWiFiConnected() {
  beepCount(2);
}
void beepMQTTConnected() {
  beepCount(3);
}
void beepWiFiLost() {
  beepLong();
}

// Portal: gentle periodic indicator (not SOS)
void beepPortalTick()    {
  buzzerBeep(120);
}

// ================= RELAYS =================
void applyLoadLevel(LoadLevel lvl) {
  digitalWrite(PIN_RELAY_1, lvl >= LOAD_LOW  ? RELAY_ON : RELAY_OFF);
  digitalWrite(PIN_RELAY_2, lvl >= LOAD_MED  ? RELAY_ON : RELAY_OFF);
  digitalWrite(PIN_RELAY_3, lvl >= LOAD_HIGH ? RELAY_ON : RELAY_OFF);
  currentLoad = lvl;
}

const char* pmsErrorToString(PmsErrorReason reason) {
  switch (reason) {
    case PMS_ERROR_NONE: return "none";
    case PMS_ERROR_TIMEOUT: return "timeout";
    case PMS_ERROR_HEADER: return "header";
    case PMS_ERROR_LENGTH: return "length";
    case PMS_ERROR_CHECKSUM: return "checksum";
    default: return "unknown";
  }
}

bool loadRebootGuardState() {
  if (!system_rtc_mem_read(REBOOT_GUARD_RTC_SLOT, &rebootGuardRtc, sizeof(rebootGuardRtc))) {
    rebootGuardRtc.magic = REBOOT_GUARD_MAGIC;
    rebootGuardRtc.cooldownActive = 0;
    rebootCooldownActive = false;
    return false;
  }

  if (rebootGuardRtc.magic != REBOOT_GUARD_MAGIC) {
    rebootGuardRtc.magic = REBOOT_GUARD_MAGIC;
    rebootGuardRtc.cooldownActive = 0;
    rebootCooldownActive = false;
    return false;
  }

  rebootCooldownActive = (rebootGuardRtc.cooldownActive != 0);
  return true;
}

void saveRebootGuardState() {
  rebootGuardRtc.magic = REBOOT_GUARD_MAGIC;
  rebootGuardRtc.cooldownActive = rebootCooldownActive ? 1 : 0;
  system_rtc_mem_write(REBOOT_GUARD_RTC_SLOT, &rebootGuardRtc, sizeof(rebootGuardRtc));
}

const char* resetReasonStr() {
  const rst_info* info = system_get_rst_info();
  if (!info) return "unknown";

  switch (info->reason) {
    case REASON_DEFAULT_RST: return "power_on";
    case REASON_SOFT_RESTART: return "soft_restart";
    case REASON_WDT_RST:
    case REASON_SOFT_WDT_RST: return "watchdog";
    case REASON_EXCEPTION_RST: return "exception";
    case REASON_EXT_SYS_RST: return "external_rst";
    default: return "unknown";
  }
}

// ================= PMS =================
bool readPMS() {
  uint8_t frame[PMS_FRAME_LENGTH];
  size_t idx = 0;
  bool headerSynced = false;
  bool sawByte = false;

  lastPmsError = PMS_ERROR_TIMEOUT;
  unsigned long start = millis();

  while (millis() - start < PMS_READ_TIMEOUT_MS) {
    if (PMS_SERIAL.available() <= 0) {
      delay(0);
      continue;
    }

    int raw = PMS_SERIAL.read();
    if (raw < 0) {
      delay(0);
      continue;
    }

    sawByte = true;
    uint8_t b = static_cast<uint8_t>(raw);

    if (!headerSynced) {
      if (b == 0x42) {
        frame[0] = b;
        idx = 1;
        headerSynced = true;
      } else {
        lastPmsError = PMS_ERROR_HEADER;
      }
      continue;
    }

    if (idx == 1) {
      if (b != 0x4D) {
        headerSynced = false;
        idx = 0;
        lastPmsError = PMS_ERROR_HEADER;
        continue;
      }
    }

    frame[idx++] = b;

    if (idx >= PMS_FRAME_LENGTH) {
      break;
    }
  }

  if (idx < PMS_FRAME_LENGTH) {
    if (!sawByte) {
      lastPmsError = PMS_ERROR_TIMEOUT;
    }
    return false;
  }

  uint16_t frameLen = (static_cast<uint16_t>(frame[2]) << 8) | frame[3];
  if (frameLen != PMS_EXPECTED_DATA_LEN) {
    lastPmsError = PMS_ERROR_LENGTH;
    return false;
  }

  uint16_t checksum = 0;
  for (uint8_t i = 0; i < 30; i++) {
    checksum += frame[i];
  }

  uint16_t frameChecksum = (static_cast<uint16_t>(frame[30]) << 8) | frame[31];
  if (checksum != frameChecksum) {
    lastPmsError = PMS_ERROR_CHECKSUM;
    return false;
  }

  telemetry.pm25 = (static_cast<uint16_t>(frame[12]) << 8) | frame[13];
  telemetry.pm10 = (static_cast<uint16_t>(frame[14]) << 8) | frame[15];
  lastPmsError = PMS_ERROR_NONE;
  return true;
}

// ================= EEPROM =================
void initDefaultWiFiStore() {
  memset(&wifiStore, 0, sizeof(wifiStore));
  wifiStore.magic = WIFI_STORE_MAGIC;
  wifiStore.lastGoodIndex = 0;
  for (uint8_t i = 0; i < MAX_WIFI_NETWORKS; i++) {
    wifiStore.nets[i].valid = 0;
  }
}

void loadWiFiCreds() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, wifiStore);
  EEPROM.end();

  if (wifiStore.magic != WIFI_STORE_MAGIC) {
    DEBUG_SERIAL.println("WiFi store not initialized; resetting EEPROM layout.");
    initDefaultWiFiStore();
    EEPROM.begin(EEPROM_SIZE);
    EEPROM.put(0, wifiStore);
    EEPROM.commit();
    EEPROM.end();
  }
}

void saveWiFiStore() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(0, wifiStore);
  EEPROM.commit();
  EEPROM.end();
}

void saveWiFiCreds(const char* ssid, const char* pass) {
  if (!ssid || ssid[0] == '\0') return;

  // If SSID already exists -> update password
  for (uint8_t i = 0; i < MAX_WIFI_NETWORKS; i++) {
    if (wifiStore.nets[i].valid && strncmp(wifiStore.nets[i].ssid, ssid, sizeof(wifiStore.nets[i].ssid)) == 0) {
      strncpy(wifiStore.nets[i].pass, pass ? pass : "", sizeof(wifiStore.nets[i].pass) - 1);
      wifiStore.nets[i].pass[sizeof(wifiStore.nets[i].pass) - 1] = '\0';
      saveWiFiStore();
      DEBUG_SERIAL.print("WiFi credentials updated for SSID: ");
      DEBUG_SERIAL.println(ssid);
      return;
    }
  }

  // Find empty slot
  for (uint8_t i = 0; i < MAX_WIFI_NETWORKS; i++) {
    if (!wifiStore.nets[i].valid) {
      memset(&wifiStore.nets[i], 0, sizeof(WiFiCredEntry));
      strncpy(wifiStore.nets[i].ssid, ssid, sizeof(wifiStore.nets[i].ssid) - 1);
      strncpy(wifiStore.nets[i].pass, pass ? pass : "", sizeof(wifiStore.nets[i].pass) - 1);
      wifiStore.nets[i].valid = 1;
      saveWiFiStore();
      DEBUG_SERIAL.print("WiFi credentials saved in slot ");
      DEBUG_SERIAL.print(i);
      DEBUG_SERIAL.print(" for SSID: ");
      DEBUG_SERIAL.println(ssid);
      return;
    }
  }

  // If full, overwrite in round-robin manner (simple policy)
  static uint8_t rr = 0;
  uint8_t idx = rr % MAX_WIFI_NETWORKS;
  rr++;

  memset(&wifiStore.nets[idx], 0, sizeof(WiFiCredEntry));
  strncpy(wifiStore.nets[idx].ssid, ssid, sizeof(wifiStore.nets[idx].ssid) - 1);
  strncpy(wifiStore.nets[idx].pass, pass ? pass : "", sizeof(wifiStore.nets[idx].pass) - 1);
  wifiStore.nets[idx].valid = 1;

  saveWiFiStore();
  DEBUG_SERIAL.print("WiFi list full; overwrote slot ");
  DEBUG_SERIAL.print(idx);
  DEBUG_SERIAL.print(" with SSID: ");
  DEBUG_SERIAL.println(ssid);
}

// ================= TIME =================
bool getEpochAndValidity(uint32_t &epochOut, bool &validOut) {
  time_t now;
  time(&now);
  epochOut = (uint32_t)now;
  validOut = (now > 1700000000); // ~2023+
  return true;
}

// ================= STATUS PUBLISH =================
void publishStatusJson(StaticJsonDocument<256> &doc) {
  if (!mqtt.connected()) return;
  char buf[256];
  serializeJson(doc, buf);
  mqtt.publish(topicStatus, buf);
}

void publishStatusEvent(const char* eventName) {
  if (!mqtt.connected()) return;

  uint32_t epoch; bool tValid;
  getEpochAndValidity(epoch, tValid);

  StaticJsonDocument<256> doc;
  doc["timestamp"] = epoch;
  doc["timeValid"] = tValid;
  doc["deviceID"]  = deviceIdHex;
  doc["event"]     = eventName;
  if (!strcmp(eventName, "boot_ok")) {
    doc["resetReason"] = resetReasonStr();
    doc["fwVersion"] = FW_VERSION;
  }

  publishStatusJson(doc);
}

void publishBootOkIfNeeded() {
  if (bootOkPublished || !mqtt.connected()) return;
  publishStatusEvent("boot_ok");
  bootOkPublished = true;
}

// publish sensor_fault with details
void publishSensorFault(bool dhtOk, bool pmsOk, bool pmsReadOk) {
  if (!mqtt.connected()) return;

  uint32_t epoch; bool tValid;
  getEpochAndValidity(epoch, tValid);

  StaticJsonDocument<256> st;
  st["timestamp"] = epoch;
  st["timeValid"] = tValid;
  st["deviceID"]  = deviceIdHex;
  st["event"]     = "sensor_fault";
  st["details"]["dhtOk"] = dhtOk;
  st["details"]["pmsOk"] = pmsOk;
  if (!pmsReadOk) {
    st["details"]["pmsError"] = pmsErrorToString(lastPmsError);
  }

  publishStatusJson(st);
}

// ================= SANITY CHECKS =================
bool dhtSanity(float t, float h) {
  if (isnan(t) || isnan(h)) return false;
  if (t < -40.0f || t > 80.0f) return false;
  if (h < 0.0f   || h > 100.0f) return false;
  return true;
}

bool pmsSanity(uint16_t pm25, uint16_t pm10) {
  if (pm25 > 2000) return false;
  if (pm10 > 2000) return false;
  return true;
}

// ================= WINDOW HELPERS =================
void resetWindow() {
  dhtSampleCount = dhtValidCount = 0;
  sumTemp = 0.0f;
  sumHum  = 0.0f;

  pmsSampleCount = pmsValidCount = 0;
  sumPM25 = 0;
  sumPM10 = 0;
}

bool windowHealthy(uint8_t validCount, uint8_t sampleCount) {
  if (sampleCount == 0) return false;
  float ratio = (float)validCount / (float)sampleCount;
  return (ratio >= VALID_RATIO_THRESHOLD);
}

// ================= PROVISIONING =================
void stopProvisioningPortal() {
  if (!provisioningActive) return;

  DEBUG_SERIAL.println("Stopping provisioning portal...");
  dnsServer.stop();
  webServer.stop();

  // Keep STA enabled; turn off AP
  WiFi.softAPdisconnect(true);
  provisioningActive = false;
}

void startProvisioningPortal() {
  provisioningActive = true;

  String apName = "SmartPole-" + String(ESP.getChipId(), HEX);

  // keep trying saved networks while portal is active
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apName.c_str());

  dnsServer.start(53, "*", WiFi.softAPIP());

  webServer.on("/", HTTP_GET, []() {
    String page;
    page += "<h2>SmartPole WiFi Setup</h2>";
    page += "<p>Enter SSID/password. This adds/updates the saved WiFi list.</p>";
    page += "<form method='POST' action='/save'>";
    page += "SSID:<br><input name='s'><br>";
    page += "Password:<br><input name='p' type='password'><br><br>";
    page += "<input type='submit' value='Save'>";
    page += "</form>";
    page += "<hr><h3>Saved networks</h3><ul>";

    for (uint8_t i = 0; i < MAX_WIFI_NETWORKS; i++) {
      if (wifiStore.nets[i].valid) {
        page += "<li>";
        page += String(i) + ": " + String(wifiStore.nets[i].ssid);
        if (i == wifiStore.lastGoodIndex) page += " (last good)";
        page += "</li>";
      }
    }
    page += "</ul>";
    webServer.send(200, "text/html", page);
  });

  webServer.on("/save", HTTP_POST, []() {
    saveWiFiCreds(webServer.arg("s").c_str(), webServer.arg("p").c_str());
    webServer.send(200, "text/html", "<h3>Saved. Device will keep trying to connect automatically.</h3><p>You can close this page.</p>");
  });

  webServer.begin();

  DEBUG_SERIAL.println("Provisioning portal active (AP+STA)");
  lastPortalBeepMillis = 0;        // force immediate tick on next loop
  lastPortalWiFiRetryMillis = 0;   // force immediate retry on next loop
}

// ================= WIFI =================
// Attempts one network with bounded wait (~10s)
bool connectWiFiTo(const char* ssid, const char* pass) {
  if (!ssid || ssid[0] == '\0') return false;

  // clear disconnect flag when starting a new attempt
  staDisconnected = false;

  WiFi.mode(provisioningActive ? WIFI_AP_STA : WIFI_STA);
  WiFi.begin(ssid, pass ? pass : "");

  DEBUG_SERIAL.print("Connecting to WiFi: ");
  DEBUG_SERIAL.println(ssid);

  const unsigned long start = millis();
  unsigned long lastDot = 0;

  while (!staDisconnected && WiFi.status() != WL_CONNECTED && (millis() - start) < 10000UL) {
    if (millis() - lastDot >= 500) {
      lastDot = millis();
      DEBUG_SERIAL.print(".");
    }
    yield();
    delay(10);
  }
  DEBUG_SERIAL.println();

  if (WiFi.status() == WL_CONNECTED && !staDisconnected) {
    DEBUG_SERIAL.print("WiFi OK, IP=");
    DEBUG_SERIAL.println(WiFi.localIP());
    wifiLostBeeped = false;
    beepWiFiConnected();
    return true;
  }

  wl_status_t st = WiFi.status();
  DEBUG_SERIAL.print("WiFi connect failed, status=");
  DEBUG_SERIAL.print((int)st);
  if (staDisconnected) DEBUG_SERIAL.print(" (event-disconnected)");
  DEBUG_SERIAL.println();
  return false;
}

// Try all saved networks, starting from last good, then round-robin.
// Returns true on success and updates lastGoodIndex.
bool connectWiFiAny() {
  uint8_t validCount = 0;
  for (uint8_t i = 0; i < MAX_WIFI_NETWORKS; i++) {
    if (wifiStore.nets[i].valid) validCount++;
  }
  if (validCount == 0) {
    DEBUG_SERIAL.println("No saved WiFi networks.");
    return false;
  }

  uint8_t startIdx = wifiStore.lastGoodIndex;
  for (uint8_t offset = 0; offset < MAX_WIFI_NETWORKS; offset++) {
    uint8_t idx = (startIdx + offset) % MAX_WIFI_NETWORKS;
    if (!wifiStore.nets[idx].valid) continue;

    if (connectWiFiTo(wifiStore.nets[idx].ssid, wifiStore.nets[idx].pass)) {
      wifiStore.lastGoodIndex = idx;
      saveWiFiStore();
      return true;
    }
  }

  return false;
}

bool connectWiFi() {
  return connectWiFiAny();
}

// ================= NTP =================
void syncTime() {
  DEBUG_SERIAL.println("Syncing NTP time...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
}

// ================= TLS =================
void initTLS() {
  secureClient.setTrustAnchors(&rootCaCert);
  secureClient.setClientRSACert(
    new BearSSL::X509List(DEVICE_CERT),
    new BearSSL::PrivateKey(PRIVATE_KEY)
  );
  secureClient.setBufferSizes(512, 512);
  ESPhttpUpdate.rebootOnUpdate(true);
}

bool fetchOtaManifest(String &newVersion, String &binUrl) {
  HTTPClient http;
  WiFiClient manifestClient;
  if (!http.begin(manifestClient, OTA_MANIFEST_URL)) {
    return false;
  }

  http.setTimeout(8000);
  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  StaticJsonDocument<1024> manifest;
  if (deserializeJson(manifest, payload)) {
    return false;
  }

  const char* version = manifest["version"];
  const char* url = manifest["bin_url"];
  if (!version || !url || strlen(version) == 0 || strlen(url) == 0) {
    return false;
  }

  newVersion = version;
  newVersion.trim();
  if (newVersion.startsWith("v") || newVersion.startsWith("V")) {
    newVersion.remove(0, 1);
  }
  binUrl = url;
  binUrl.trim();
  if (!binUrl.startsWith("http://")) {
    return false;
  }
  return true;
}

void handleOtaUpdate() {
  if (!otaUpdateRequested) return;

  if (!mqtt.connected() || staDisconnected || WiFi.status() != WL_CONNECTED) {
    otaUpdateRequested = false;
    return;
  }

  otaUpdateRequested = false;
  String newVersion;
  String binUrl;
  if (!fetchOtaManifest(newVersion, binUrl)) {
    StaticJsonDocument<256> st;
    uint32_t epoch; bool tValid;
    getEpochAndValidity(epoch, tValid);

    st["timestamp"] = epoch;
    st["timeValid"] = tValid;
    st["deviceID"]  = deviceIdHex;
    st["event"]     = "ota_failed";
    st["error"]     = "manifest_fetch_failed";
    publishStatusJson(st);
    return;
  }

  String currentVersion = FW_VERSION;
  currentVersion.trim();
  if (currentVersion.startsWith("v") || currentVersion.startsWith("V")) {
    currentVersion.remove(0, 1);
  }
  if (newVersion == currentVersion) {
    publishStatusEvent("ota_no_updates");
    return;
  }

  publishStatusEvent("ota_start");
  DEBUG_SERIAL.print("OTA downloading from: ");
  DEBUG_SERIAL.println(binUrl);

  WiFiClient updateClient;
  t_httpUpdate_return ret = ESPhttpUpdate.update(updateClient, binUrl);
  DEBUG_SERIAL.print("OTA update result: ");
  DEBUG_SERIAL.println((int)ret);
  if (ret == HTTP_UPDATE_FAILED) {
    StaticJsonDocument<256> st;
    uint32_t epoch; bool tValid;
    getEpochAndValidity(epoch, tValid);

    st["timestamp"] = epoch;
    st["timeValid"] = tValid;
    st["deviceID"]  = deviceIdHex;
    st["event"]     = "ota_failed";
    st["error"]     = ESPhttpUpdate.getLastErrorString();
    st["code"]      = ESPhttpUpdate.getLastError();
    DEBUG_SERIAL.print("OTA failed code: ");
    DEBUG_SERIAL.println(ESPhttpUpdate.getLastError());
    DEBUG_SERIAL.print("OTA failed error: ");
    DEBUG_SERIAL.println(ESPhttpUpdate.getLastErrorString());
    publishStatusJson(st);
    return;
  }

  DEBUG_SERIAL.println("OTA update succeeded; rebooting...");
  // HTTP_UPDATE_OK: device reboots automatically (rebootOnUpdate=true).
  // Confirmation is emitted on next boot via boot_ok with fwVersion.
}

// ================= MQTT CALLBACK =================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  static unsigned long lastRebootCommandMillis = 0;
  static char msg[256];
  if (length >= sizeof(msg)) return;
  memcpy(msg, payload, length);
  msg[length] = '\0';

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, msg)) return;

  if (doc.containsKey("event")) return;

  const char* cmd = doc["command"];
  if (!cmd) return;

  if (!strcmp(cmd, "set_load")) {
    int val = doc["value"] | -1;
    if (val >= 0 && val <= 3) {
      applyLoadLevel((LoadLevel)val);

      uint32_t epoch; bool tValid;
      getEpochAndValidity(epoch, tValid);

      StaticJsonDocument<256> ack;
      ack["timestamp"] = epoch;
      ack["timeValid"] = tValid;
      ack["deviceID"]  = deviceIdHex;
      ack["event"]     = "command_ack";
      ack["command"]   = "set_load";
      ack["value"]     = val;
      ack["result"]    = "ok";
      ack["load"]      = currentLoad;

      publishStatusJson(ack);
    }
    return;
  }

  if (!strcmp(cmd, "beep")) {
    beepCount(2);

    uint32_t epoch; bool tValid;
    getEpochAndValidity(epoch, tValid);

    StaticJsonDocument<256> ack;
    ack["timestamp"] = epoch;
    ack["timeValid"] = tValid;
    ack["deviceID"]  = deviceIdHex;
    ack["event"]     = "command_ack";
    ack["command"]   = "beep";
    ack["result"]    = "ok";

    publishStatusJson(ack);
    return;
  }

  if (!strcmp(cmd, "reboot")) {
    unsigned long nowMs = millis();
    if (rebootCooldownActive) {
      if (nowMs < REBOOT_GUARD_WINDOW_MS) return;
      rebootCooldownActive = false;
      saveRebootGuardState();
    }

    if (lastRebootCommandMillis != 0 && (nowMs - lastRebootCommandMillis) < REBOOT_GUARD_WINDOW_MS) return;
    lastRebootCommandMillis = nowMs;

    uint32_t epoch; bool tValid;
    getEpochAndValidity(epoch, tValid);

    StaticJsonDocument<256> ack;
    ack["timestamp"] = epoch;
    ack["timeValid"] = tValid;
    ack["deviceID"]  = deviceIdHex;
    ack["event"]     = "command_ack";
    ack["command"]   = "reboot";
    ack["result"]    = "ok";
    publishStatusJson(ack);

    rebootCooldownActive = true;
    saveRebootGuardState();

    delay(200);
    ESP.restart();
    return;
  }

  if (!strcmp(cmd, "ota_update")) {
    unsigned long nowMs = millis();
    if (lastOtaCommandMillis != 0 && (nowMs - lastOtaCommandMillis) < OTA_COMMAND_GUARD_MS) return;
    if (!mqtt.connected()) return;

    lastOtaCommandMillis = nowMs;
    otaUpdateRequested = true;
    return;
  }
}

// ================= MQTT =================
void connectMQTT() {
  if (staDisconnected || WiFi.status() != WL_CONNECTED) {
    DEBUG_SERIAL.println("Skipping MQTT connect: WiFi not connected (event/status).");
    return;
  }

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  char clientId[32];
  snprintf(clientId, sizeof(clientId), "smartpole-%06X", ESP.getChipId());

  DEBUG_SERIAL.print("Connecting to MQTT as ");
  DEBUG_SERIAL.println(clientId);

  for (uint8_t attempt = 1; attempt <= 5 && !mqtt.connected(); attempt++) {
    // Re-check WiFi each attempt to avoid wasting time when WiFi drops mid-loop.
    if (staDisconnected || WiFi.status() != WL_CONNECTED) {
      DEBUG_SERIAL.println("MQTT connect aborted: WiFi dropped during MQTT attempts.");
      return;
    }

    bool ok = mqtt.connect(clientId);
    DEBUG_SERIAL.print("MQTT attempt ");
    DEBUG_SERIAL.print(attempt);
    DEBUG_SERIAL.print("/5 => ");
    DEBUG_SERIAL.print(ok ? "OK" : "FAIL");
    DEBUG_SERIAL.print(" (state=");
    DEBUG_SERIAL.print(mqtt.state());
    DEBUG_SERIAL.println(")");

    if (!ok) {
      yield();
      delay(2000);
    }
  }

  if (!mqtt.connected()) {
    DEBUG_SERIAL.println("MQTT connect failed (giving up for now).");
    return;
  }

  DEBUG_SERIAL.println("MQTT connected");
  beepMQTTConnected();

  mqtt.subscribe(topicStatus);
  publishStatusEvent("mqtt_connected");
  publishBootOkIfNeeded();

  // If in portal mode and MQTT is now up, stop portal
  // (WiFi may have connected while portal was active.)
  if (provisioningActive) stopProvisioningPortal();
}

// ================= SETUP =================
void setup() {
  PMS_SERIAL.begin(9600);
  DEBUG_SERIAL.begin(115200);
  delay(2000);

  DEBUG_SERIAL.println("\nBOOT STARTED");

  loadRebootGuardState();

  onStaDisconnected = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected & event) {
    staDisconnected = true;
    DEBUG_SERIAL.print("WiFi DISCONNECTED (reason=");
    DEBUG_SERIAL.print((int)event.reason);
    DEBUG_SERIAL.println(")");
  });

  onStaGotIP = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP & event) {
    staDisconnected = false;
    DEBUG_SERIAL.print("WiFi GOT IP: ");
    DEBUG_SERIAL.println(WiFi.localIP());
  });

  pinMode(PIN_RELAY_1, OUTPUT); digitalWrite(PIN_RELAY_1, RELAY_OFF);
  pinMode(PIN_RELAY_2, OUTPUT); digitalWrite(PIN_RELAY_2, RELAY_OFF);
  pinMode(PIN_RELAY_3, OUTPUT); digitalWrite(PIN_RELAY_3, RELAY_OFF);
  pinMode(PIN_BUZZER, OUTPUT);  digitalWrite(PIN_BUZZER, HIGH);
  beepBootOK();

  dht.begin();

  telemetry.deviceID = ESP.getChipId();
  snprintf(deviceIdHex, sizeof(deviceIdHex), "%06X", telemetry.deviceID);

  snprintf(topicRegister, sizeof(topicRegister), "smart_pole/device_registration");
  snprintf(topicData, sizeof(topicData), "smart_pole/update/%s/data", deviceIdHex);
  snprintf(topicStatus, sizeof(topicStatus), "smart_pole/update/%s/status", deviceIdHex);

  DEBUG_SERIAL.print("Topic data: ");
  DEBUG_SERIAL.println(topicData);
  DEBUG_SERIAL.print("Topic status: ");
  DEBUG_SERIAL.println(topicStatus);

  loadWiFiCreds();

  // Try connecting to any saved WiFi. If it fails, start portal (but keep trying in background).
  if (!connectWiFi()) {
    startProvisioningPortal();
  } else {
    publishStatusEvent("wifi_connected");
  }

  syncTime();
  initTLS();
  connectMQTT(); // bounded, WiFi-first
  publishBootOkIfNeeded();

  // ---- DEVICE REGISTRATION ----
  StaticJsonDocument<256> reg;
  reg["deviceId"]   = deviceIdHex;
  reg["fwVersion"]  = FW_VERSION;
  reg["hwVersion"]  = HW_VERSION;

  char buf[256];
  serializeJson(reg, buf);
  if (mqtt.connected()) mqtt.publish(topicRegister, buf);

  resetWindow();
  lastSampleMillis  = millis();
  lastPublishMillis = millis();

}

// ================= LOOP =================
void loop() {
  unsigned long nowMillis = millis();

  // If there's a disconnect event, treat WiFi as down immediately
  if (staDisconnected) {
    if (mqtt.connected()) mqtt.disconnect();
  }

  // Always service portal if active (DNS/Web), but DO NOT block reconnection attempts.
  if (provisioningActive) {
    dnsServer.processNextRequest();
    webServer.handleClient();

    // periodic portal indicator
    if (nowMillis - lastPortalBeepMillis >= PORTAL_BEEP_INTERVAL_MS) {
      lastPortalBeepMillis = nowMillis;
      beepPortalTick();
    }

    // background WiFi retry while portal is open
    if (nowMillis - lastPortalWiFiRetryMillis >= PORTAL_WIFI_RETRY_INTERVAL_MS) {
      lastPortalWiFiRetryMillis = nowMillis;
      DEBUG_SERIAL.println("Portal mode: trying saved WiFi networks in background...");
      if (connectWiFiAny()) {
        DEBUG_SERIAL.println("Portal mode: WiFi connected. Continuing normal operation.");
        publishStatusEvent("wifi_connected");
        syncTime();
        connectMQTT(); // will stop portal if MQTT connects
      }
    }
    // continue execution even if portal is active (do not return)
  }

  // ---- WiFi reconnection (WiFi FIRST, before MQTT) ----
  if (staDisconnected || WiFi.status() != WL_CONNECTED) {
    if (!wifiLostBeeped) {
      beepWiFiLost();
      wifiLostBeeped = true;
    }

    // Ensure MQTT is not wasting time while WiFi is down
    if (mqtt.connected()) mqtt.disconnect();

    // If portal is already active, background retry above handles it.
    // If portal is not active, attempt reconnection a few cycles then open portal.
    if (!provisioningActive) {
      if (++wifiFailCount >= WIFI_MAX_RETRIES) {
        publishStatusEvent("wifi_lost");
        startProvisioningPortal();
        wifiFailCount = 0;
        return;
      }

      if (connectWiFiAny()) {
        wifiFailCount = 0;
        publishStatusEvent("wifi_connected");
        syncTime();
      }
    }
    return;
  }
  wifiFailCount = 0;
  wifiLostBeeped = false;

  // ---- MQTT reconnect (only when WiFi is up) ----
  if (!mqtt.connected()) connectMQTT();
  mqtt.loop();
  handleOtaUpdate();

  // ---- Sample every 10 seconds ----
  if (nowMillis - lastSampleMillis >= SAMPLE_INTERVAL_MS) {
    lastSampleMillis = nowMillis;

    float t = dht.readTemperature();
    float h = dht.readHumidity();
    bool dhtOk = dhtSanity(t, h);

    if (dhtWarmupStartMillis == 0) {
      dhtWarmupStartMillis = nowMillis;
    }

    bool dhtWarmupActive = !dhtHasValidSample
      && (nowMillis - dhtWarmupStartMillis < DHT_WARMUP_MS);

    if (!dhtWarmupActive || dhtOk) {
      dhtSampleCount++;
      if (dhtOk) {
        dhtValidCount++;
        sumTemp += t;
        sumHum  += h;
        dhtHasValidSample = true;
      }
    }

    bool pmsReadOk = readPMS();
    bool pmsOk = pmsReadOk && pmsSanity(telemetry.pm25, telemetry.pm10);

    pmsSampleCount++;
    if (pmsOk) {
      pmsValidCount++;
      sumPM25 += telemetry.pm25;
      sumPM10 += telemetry.pm10;
    }

    // Failure detection remains per-sample (every 10s)
    bool dhtOkForFault = dhtWarmupActive ? true : dhtOk;
    bool sampleHealthy = dhtOkForFault && pmsOk;

    if (!sampleHealthy) {
      bool wasFaultActive = sensorFaultActive;
      sensorFaultActive = true;

      if (!wasFaultActive || nowMillis - lastSensorFaultBeepMillis >= SENSOR_FAULT_BEEP_INTERVAL_MS) {
        lastSensorFaultBeepMillis = nowMillis;
        beepRapid5();
      }

      publishSensorFault(dhtOkForFault, pmsOk, pmsReadOk);
    } else {
      if (sensorFaultActive) {
        sensorFaultActive = false;
        publishStatusEvent("sensor_recovered");
      }
    }
  }

  // ---- Publish every 60 seconds (summary window) ----
  if (nowMillis - lastPublishMillis >= PUBLISH_INTERVAL_MS) {
    lastPublishMillis = nowMillis;

    uint32_t epoch; bool tValid;
    getEpochAndValidity(epoch, tValid);

    telemetry.timestamp = epoch;
    telemetry.timeValid = tValid;
    telemetry.load      = currentLoad;

    bool dhtHealthy = windowHealthy(dhtValidCount, dhtSampleCount);
    bool pmsHealthy = windowHealthy(pmsValidCount, pmsSampleCount);
    telemetry.sensorValid = dhtHealthy && pmsHealthy;

    if (dhtValidCount > 0) {
      telemetry.temperature = sumTemp / (float)dhtValidCount;
      telemetry.humidity    = sumHum  / (float)dhtValidCount;
    } else {
      telemetry.temperature = NAN;
      telemetry.humidity    = NAN;
    }

    if (pmsValidCount > 0) {
      telemetry.pm25 = (uint16_t)(sumPM25 / (uint32_t)pmsValidCount);
      telemetry.pm10 = (uint16_t)(sumPM10 / (uint32_t)pmsValidCount);
    } else {
      telemetry.pm25 = 0;
      telemetry.pm10 = 0;
    }

    StaticJsonDocument<256> doc;
    doc["timestamp"]   = telemetry.timestamp;
    doc["timeValid"]   = telemetry.timeValid;
    doc["deviceID"]    = deviceIdHex;
    doc["temperature"] = telemetry.temperature;
    doc["humidity"]    = telemetry.humidity;
    doc["pm25"]        = telemetry.pm25;
    doc["pm10"]        = telemetry.pm10;
    doc["sensorValid"] = telemetry.sensorValid;
    doc["load"]        = telemetry.load;

    char payload[256];
    serializeJson(doc, payload);

    if (mqtt.connected()) {
      bool ok = mqtt.publish(topicData, payload);
      DEBUG_SERIAL.print("Publishing telemetry to ");
      DEBUG_SERIAL.println(topicData);
      DEBUG_SERIAL.println(payload);
      DEBUG_SERIAL.print("Publish result: ");
      DEBUG_SERIAL.println(ok ? "OK" : "FAILED");
    } else {
      DEBUG_SERIAL.println("Telemetry skipped: MQTT not connected");
    }

    resetWindow();
  }

  // ---- Persistent error beep (STATE_ERROR use) ----
  if (systemState == STATE_ERROR) {
    if (nowMillis - lastErrorBeepMillis >= ERROR_BEEP_INTERVAL_MS) {
      lastErrorBeepMillis = nowMillis;
      beepLong();
      publishStatusEvent("error_persist");
    }
  }
}
