// =============================================================================
// =============  ESS POC FIRMWARE (FULL MERGED - RS485 RECOVERY)  =============
// =============================================================================
// Features:
//  - Smart WiFi manager (NVS, AP portal, double-reset detect) - unchanged
//  - Blynk, OTA only after STA success - unchanged
//  - RS485: HALFDUPLEX + robust recovery (reinit on write & on repeated read failures)
//  - Manual Modbus console via Blynk (V20..V24) with empty-input guard
//  - Fan relays (R1_1, R1_2, R1_3)
//  - Per-register scaling, batch helper, CRC, etc.
//
// Notes:
//  - readInterval is 5s (good for POC). Increase if you want lower traffic.
//  - You can adjust RS485_RECOVER_THRESHOLD to tune auto-recovery sensitivity.
// =============================================================================

// -------------------------- Blynk ---------------------------------
#define BLYNK_TEMPLATE_ID "TMPL6N8BMryWD"
#define BLYNK_TEMPLATE_NAME "ESS"
#define BLYNK_AUTH_TOKEN  "AKuvYfwOjNbXiIZLqej5XoT9yHcg2bs7"

#include <RS485.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <BlynkSimpleEsp32.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <WebServer.h>

// =============================================================================
//                     SMART WIFI MANAGER SECTION (UNCHANGED)
// =============================================================================

Preferences prefs;
WebServer server(80);

const uint8_t MAX_WIFI_NETWORKS = 5;
const uint8_t WIFI_MAX_RETRIES = 3;

String storedSSID[MAX_WIFI_NETWORKS];
String storedPASS[MAX_WIFI_NETWORKS];
bool wifiValid[MAX_WIFI_NETWORKS];
uint8_t lastGoodWiFiIndex = 0;

bool forceConfig = false;
bool inConfigPortal = false;
bool blynkOtaInitialized = false;

unsigned long lastConnectAttempt = 0;
const unsigned long connectRetryInterval = 10000;
uint8_t wifiFailCount = 0;

bool reconnectInProgress = false;
uint8_t reconnectStartIdx = 0;
uint8_t reconnectOffset = 0;
uint8_t reconnectCurrentIdx = 0;
unsigned long reconnectAttemptStarted = 0;
const unsigned long reconnectAttemptTimeout = 10000;

RTC_DATA_ATTR int bootCount = 0;
unsigned long resetTimeWindow = 4000;

// -------- Load saved credentials --------
void loadCredentials() {
  prefs.begin("wifi", true);
  lastGoodWiFiIndex = prefs.getUChar("lastgood", 0);
  bool hasSlotProfiles = false;
  for (uint8_t i = 0; i < MAX_WIFI_NETWORKS; i++) {
    String validKey = "valid" + String(i);
    String ssidKey = "ssid" + String(i);
    String passKey = "pass" + String(i);
    wifiValid[i] = prefs.getBool(validKey.c_str(), false);
    storedSSID[i] = prefs.getString(ssidKey.c_str(), "");
    storedPASS[i] = prefs.getString(passKey.c_str(), "");
    if (!wifiValid[i] || storedSSID[i].length() == 0) {
      wifiValid[i] = false;
      storedSSID[i] = "";
      storedPASS[i] = "";
    } else {
      hasSlotProfiles = true;
    }
  }

  // Backward-compatibility: migrate legacy single-profile keys.
  if (!hasSlotProfiles) {
    String legacySSID = prefs.getString("ssid", "");
    String legacyPASS = prefs.getString("pass", "");
    legacySSID.trim();
    if (legacySSID.length() > 0) {
      wifiValid[0] = true;
      storedSSID[0] = legacySSID;
      storedPASS[0] = legacyPASS;
      lastGoodWiFiIndex = 0;
    }
  }
  prefs.end();

  if (!hasSlotProfiles && wifiValid[0]) {
    saveWiFiStore();
    Serial.println("Migrated legacy WiFi credentials to slot 0.");
  }

  Serial.println("Loaded WiFi profiles:");
  for (uint8_t i = 0; i < MAX_WIFI_NETWORKS; i++) {
    if (wifiValid[i]) {
      Serial.print("  ");
      Serial.print(i);
      Serial.print(": ");
      Serial.print(storedSSID[i]);
      if (i == lastGoodWiFiIndex) Serial.print(" (last good)");
      Serial.println();
    }
  }
}

void saveWiFiStore() {
  prefs.begin("wifi", false);
  prefs.putUChar("lastgood", lastGoodWiFiIndex);
  for (uint8_t i = 0; i < MAX_WIFI_NETWORKS; i++) {
    String validKey = "valid" + String(i);
    String ssidKey = "ssid" + String(i);
    String passKey = "pass" + String(i);
    prefs.putBool(validKey.c_str(), wifiValid[i]);
    prefs.putString(ssidKey.c_str(), storedSSID[i]);
    prefs.putString(passKey.c_str(), storedPASS[i]);
  }
  prefs.end();
}

// -------- Save new credentials --------
void saveCredentials(String ssid, String pass) {
  ssid.trim();
  if (ssid.length() == 0) return;

  // Update existing SSID
  for (uint8_t i = 0; i < MAX_WIFI_NETWORKS; i++) {
    if (wifiValid[i] && storedSSID[i] == ssid) {
      storedPASS[i] = pass;
      saveWiFiStore();
      Serial.println("Updated WiFi credentials for SSID: " + ssid);
      return;
    }
  }

  // Save in empty slot
  for (uint8_t i = 0; i < MAX_WIFI_NETWORKS; i++) {
    if (!wifiValid[i]) {
      wifiValid[i] = true;
      storedSSID[i] = ssid;
      storedPASS[i] = pass;
      saveWiFiStore();
      Serial.println("Saved WiFi credentials in slot " + String(i) + ": " + ssid);
      return;
    }
  }

  // If full, overwrite next slot (round-robin)
  static uint8_t rr = 0;
  uint8_t idx = rr % MAX_WIFI_NETWORKS;
  rr++;
  wifiValid[idx] = true;
  storedSSID[idx] = ssid;
  storedPASS[idx] = pass;
  saveWiFiStore();
  Serial.println("WiFi list full. Overwrote slot " + String(idx) + " with SSID: " + ssid);
}

// -------- Captive Portal HTML --------
void handleRoot() {
  String html = "<html><body>"
                "<h2>ESP32 PLC WiFi Config</h2>"
                "<p>Enter SSID/password to add or update a WiFi profile.</p>"
                "<form action='/save' method='POST'>"
                "SSID:<br><input name='ssid'><br>"
                "Password:<br><input type='password' name='pass'><br><br>"
                "<input type='submit' value='Save'>"
                "</form><hr><h3>Saved networks</h3><ul>";

  for (uint8_t i = 0; i < MAX_WIFI_NETWORKS; i++) {
    if (wifiValid[i]) {
      html += "<li>" + String(i) + ": " + storedSSID[i];
      if (i == lastGoodWiFiIndex) html += " (last good)";
      html += "</li>";
    }
  }

  html += "</ul></body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  saveCredentials(server.arg("ssid"), server.arg("pass"));
  server.send(200, "text/html", "Saved. Device will keep trying networks automatically. You can close this page.");
}

// -------- Start AP Mode --------
void startConfigPortal() {
  inConfigPortal = true;

  Serial.println("Starting AP Config Portal...");
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("PLC_Config", "12345678");

  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
}

// -------- STA Connection Attempt --------
bool connectWiFiTo(const String& ssid, const String& pass) {
  if (ssid.length() == 0) return false;

  WiFi.mode(inConfigPortal ? WIFI_AP_STA : WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  Serial.println("Trying WiFi: " + ssid);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
    delay(200);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected: " + WiFi.localIP().toString());
    return true;
  }

  Serial.println("\nWiFi Failed.");
  return false;
}

bool connectWiFiAny() {
  uint8_t validCount = 0;
  for (uint8_t i = 0; i < MAX_WIFI_NETWORKS; i++) {
    if (wifiValid[i]) validCount++;
  }

  if (validCount == 0) {
    Serial.println("No saved SSID profiles.");
    return false;
  }

  uint8_t startIdx = lastGoodWiFiIndex;
  for (uint8_t offset = 0; offset < MAX_WIFI_NETWORKS; offset++) {
    uint8_t idx = (startIdx + offset) % MAX_WIFI_NETWORKS;
    if (!wifiValid[idx]) continue;
    if (connectWiFiTo(storedSSID[idx], storedPASS[idx])) {
      lastGoodWiFiIndex = idx;
      saveWiFiStore();
      return true;
    }
  }
  return false;
}

bool startReconnectAttempt(uint8_t idx) {
  if (!wifiValid[idx] || storedSSID[idx].length() == 0) return false;

  reconnectCurrentIdx = idx;
  reconnectInProgress = true;
  reconnectAttemptStarted = millis();

  WiFi.mode(inConfigPortal ? WIFI_AP_STA : WIFI_STA);
  WiFi.begin(storedSSID[idx].c_str(), storedPASS[idx].c_str());

  Serial.println("Reconnect attempt started for slot " + String(idx) + ": " + storedSSID[idx]);
  return true;
}

bool startNextReconnectAttempt() {
  while (reconnectOffset < MAX_WIFI_NETWORKS) {
    uint8_t idx = (reconnectStartIdx + reconnectOffset) % MAX_WIFI_NETWORKS;
    reconnectOffset++;
    if (startReconnectAttempt(idx)) return true;
  }
  reconnectInProgress = false;
  return false;
}

void resetReconnectState() {
  reconnectInProgress = false;
  reconnectOffset = 0;
  reconnectCurrentIdx = 0;
  reconnectAttemptStarted = 0;
}

bool tryReconnectAnyNonBlocking() {
  if (WiFi.status() == WL_CONNECTED) {
    resetReconnectState();
    return true;
  }

  if (reconnectInProgress) {
    if (millis() - reconnectAttemptStarted >= reconnectAttemptTimeout) {
      Serial.println("Reconnect attempt timed out for slot " + String(reconnectCurrentIdx));
      WiFi.disconnect();
      if (!startNextReconnectAttempt()) {
        return false;
      }
    }
    return true;
  }

  reconnectStartIdx = lastGoodWiFiIndex;
  reconnectOffset = 0;
  return startNextReconnectAttempt();
}

bool tryConnectSTA() { return connectWiFiAny(); }

void initBlynkAndOta() {
  Blynk.config(BLYNK_AUTH_TOKEN);
  Blynk.connect();

  if (!blynkOtaInitialized) {
    ArduinoOTA.setHostname("esp32-plc");
    ArduinoOTA.begin();
    Serial.println("OTA Ready.");
    blynkOtaInitialized = true;
  }

  Serial.println("WiFi MAC: " + WiFi.macAddress());
}

// =============================================================================
//                         ORIGINAL PROJECT VARIABLES
// =============================================================================

byte slaveID = 0x05;

// Register Addresses
uint16_t reg_BMSVol          = 15112;
uint16_t reg_BMSCurr         = 15113;
uint16_t reg_BMSTemp         = 15114;
uint16_t reg_BMSSOC          = 15115;
uint16_t reg_ForceChargeMode = 10120;
uint16_t reg_InvTemp         = 15107;
uint16_t reg_InverterRunStop = 20211;
uint16_t reg_Vgrid_R         = 25237;
uint16_t reg_Igrid_R         = 25240;
uint16_t reg_GridFreq        = 25243;
uint16_t reg_Vinv_R          = 25247;
uint16_t reg_Iinv_R          = 25250;
uint16_t reg_InvFreq         = 25253;

unsigned long lastReadTime = 0;
const unsigned long readInterval = 5000UL;

String regInput = "";
String valInput = "";
uint16_t manualReg = 0;
int32_t manualVal = 0;

// RS485 recovery globals
int consecutiveReadFailures = 0;
const int RS485_RECOVER_THRESHOLD = 6; // number of failed register reads before reinit
const int RS485_REINIT_DELAY_MS = 80;  // delay after re-init to allow bus settle

// -------------------------- Fan control globals & thresholds ------------
bool manualOverride = false; // manual override from Blynk V17 (ON forces all fans)
int fan1_state = LOW; // R1_1
int fan2_state = LOW; // R1_2
int fan3_state = LOW; // R1_3

// BMS thresholds with 2°C hysteresis (per your request)
const float FAN1_ON  = 35.0f;
const float FAN1_OFF = 33.0f;
const float FAN2_ON  = 40.0f;
const float FAN2_OFF = 38.0f;
const float FAN3_ON  = 45.0f;
const float FAN3_OFF = 43.0f;

// Inverter emergency thresholds
const float INV_EMERG_ON  = 60.0f;
const float INV_EMERG_OFF = 50.0f;

// -------------------------- Forward declarations -------------------------
float readModbusRegister(byte id, uint16_t reg);
bool modbusReadMultiple(byte id, uint16_t startReg, uint16_t count, uint16_t *buffer);
void sendModbusWriteSingleRegister(byte id, uint16_t reg, uint16_t val);
uint16_t calculateCRC(byte *array, byte len);
float scaleRegister(uint16_t reg);
void checkConnections();
void rs485_reinit();
void updateFansBasedOnTemps(float bmsTempC, float invTempC);

// =============================================================================
//                                   SETUP
// =============================================================================

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("=== ESS POC BOOT ===");

  // Start RS485 in half duplex (important for MAX485)
  RS485.begin(9600, HALFDUPLEX);
  Serial.println("RS485 HALFDUPLEX started.");

  // relay init
  pinMode(R1_1, OUTPUT);
  pinMode(R1_2, OUTPUT);
  pinMode(R1_3, OUTPUT);
  digitalWrite(R1_1, LOW);
  digitalWrite(R1_2, LOW);
  digitalWrite(R1_3, LOW);

  // -------- DOUBLE RESET DETECTION --------
  if (bootCount == 0) {
    bootCount = 1;
    Serial.println("Boot 1/2...");
  } else {
    Serial.println("Boot 2/2 → FORCING CONFIG MODE");
    forceConfig = true;
  }
  unsigned long t0 = millis();
  while (millis() - t0 < resetTimeWindow) delay(1);
  bootCount = 0;

  // -------- LOAD CREDENTIALS --------
  loadCredentials();

  bool wifiOK = false;
  if (!forceConfig) wifiOK = tryConnectSTA();
  if (!wifiOK) startConfigPortal();

  // -------- Start Blynk + OTA ONLY WHEN CONNECTED --------
  if (WiFi.status() == WL_CONNECTED) {
    initBlynkAndOta();
  } else {
    Serial.println("WiFi not connected. OTA unavailable until WiFi connects.");
  }

  Serial.println("Setup complete.");
}

// =============================================================================
//                                   LOOP
// =============================================================================

void loop() {
  Blynk.run();

  if (WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.handle();
  }

  // Handle captive portal when active
  if (inConfigPortal) {
    server.handleClient();
    if (millis() - lastConnectAttempt > connectRetryInterval) {
      lastConnectAttempt = millis();
      Serial.println("Portal mode: trying saved WiFi profiles...");
      if (connectWiFiAny()) {
        Serial.println("WiFi OK! Closing AP.");
        WiFi.softAPdisconnect(true);
        inConfigPortal = false;

        // bring up Blynk + OTA
        initBlynkAndOta();
      }
    }
  }

  // keep Blynk/WiFi healthy
  checkConnections();

  // Only read registers on schedule (prevents bus flooding)
  if (millis() - lastReadTime >= readInterval) {
    // Read registers (with built-in retries & scaling)
    float battVoltage      = readModbusRegister(slaveID, reg_BMSVol);
    float battCurrent      = readModbusRegister(slaveID, reg_BMSCurr);
    float battTemp         = readModbusRegister(slaveID, reg_BMSTemp); // BMS / battery temp (°C)
    float soc              = readModbusRegister(slaveID, reg_BMSSOC);
    float gridVoltage      = readModbusRegister(slaveID, reg_Vgrid_R);
    float gridCurrent      = readModbusRegister(slaveID, reg_Igrid_R);
    float gridFreq         = readModbusRegister(slaveID, reg_GridFreq);
    float invTemp          = readModbusRegister(slaveID, reg_InvTemp);  // inverter temp (°C)
    float inverterVoltage  = readModbusRegister(slaveID, reg_Vinv_R);
    float inverterCurrent  = readModbusRegister(slaveID, reg_Iinv_R);
    float inverterFreq     = readModbusRegister(slaveID, reg_InvFreq);

    // ====================== GRID PRESENCE & CHARGE CONTROL ======================
    // Threshold for grid detection
    const float GRID_PRESENT_THRESHOLD = 150.0f; // volts

    // ForceChargeMode values (from your register table)
    const uint16_t MODE_NORMAL      = 0;  // Let inverter operate normally
    const uint16_t MODE_FORCE_CHG   = 2;  // Force Charge

    // Stabilization (anti-flap) timing
    const unsigned long GRID_CONFIRM_TIME = 10000UL; // 10 seconds
    const unsigned long GRID_LOSS_TIME    = 10000UL; // 10 seconds

    // Internal static states (persist across loop calls)
    static bool gridStablePresent = false;
    static bool gridStableAbsent  = true;   // Default assume no grid on boot
    static unsigned long gridOnTimer  = 0;
    static unsigned long gridOffTimer = 0;

    static uint16_t lastSentChargeMode = 999;  // invalid start value

    // Protect against bad reads: treat error sentinel as absent
    bool validGridRead = !(gridVoltage < -1000.0f);

    // ------------------- Step 1: Detect instantaneous grid state -------------------
    bool gridNowPresent = validGridRead && (gridVoltage > GRID_PRESENT_THRESHOLD);

    // ------------------- Step 2: Stabilize grid PRESENT -------------------
    if (gridNowPresent) {
      gridOffTimer = 0;  // reset loss timer
      if (!gridStablePresent) {
        if (gridOnTimer == 0) gridOnTimer = millis();
        if (millis() - gridOnTimer >= GRID_CONFIRM_TIME) {
          gridStablePresent = true;
          gridStableAbsent  = false;
          Serial.println("[GRID] Stabilized: PRESENT");
        }
      }
    } else {
      gridOnTimer = 0;  // reset present timer
    }

    // ------------------- Step 3: Stabilize grid ABSENCE -------------------
    if (!gridNowPresent) {
      gridOnTimer = 0; // reset present timer
      if (!gridStableAbsent) {
        if (gridOffTimer == 0) gridOffTimer = millis();
        if (millis() - gridOffTimer >= GRID_LOSS_TIME) {
          gridStableAbsent  = true;
          gridStablePresent = false;
          Serial.println("[GRID] Stabilized: ABSENT");
        }
      }
    } else {
      gridOffTimer = 0;
    }

    // ------------------- Step 4: Determine desired inverter mode -------------------
    uint16_t desiredMode;

    if (gridStablePresent) {
      // Grid is stable → force charge
      desiredMode = MODE_FORCE_CHG;
    } else {
      // Grid absent or unstable → revert to normal
      desiredMode = MODE_NORMAL;
    }

    // ------------------- Step 5: Write only when there's a change -------------------
    if (desiredMode != lastSentChargeMode) {
      sendModbusWriteSingleRegister(slaveID, reg_ForceChargeMode, desiredMode);
      lastSentChargeMode = desiredMode;
      Serial.printf("[GRID-CTRL] Wrote ForceChargeMode = %u\n", desiredMode);
    }
    // ===============================================================================

    // Update fans using BMS as primary, inverter as emergency
    // Only update if we have valid measurements (readModbusRegister returns -9999 on error)
    if (battTemp > -1000.0f || invTemp > -1000.0f) {
      updateFansBasedOnTemps(battTemp, invTemp);
    } else {
      // No valid temps: do nothing to fans (maintain last state)
      Serial.println("Warning: no valid temps for fan update; skipping.");
    }

    Blynk.virtualWrite(V0,  battVoltage);
    Blynk.virtualWrite(V1,  battCurrent);
    Blynk.virtualWrite(V2,  soc);
    Blynk.virtualWrite(V3,  battTemp);
    Blynk.virtualWrite(V4,  gridVoltage);
    Blynk.virtualWrite(V5,  gridCurrent);
    Blynk.virtualWrite(V6,  gridFreq);
    Blynk.virtualWrite(V8,  invTemp);
    Blynk.virtualWrite(V12, inverterVoltage);
    Blynk.virtualWrite(V13, inverterCurrent);
    Blynk.virtualWrite(V14, inverterFreq);

    Serial.println("Blynk: values updated.");
    lastReadTime = millis();
  }
}

// =============================================================================
//                               BLYNK HANDLERS
// =============================================================================

// Inverter run/stop
BLYNK_WRITE(V15) {
  int state = param.asInt();
  Serial.printf("V15 Run/Stop: %d\n", state);
  sendModbusWriteSingleRegister(slaveID, reg_InverterRunStop, (uint16_t)state);
}

// Charge mode
BLYNK_WRITE(V16) {
  int m = param.asInt();
  Serial.printf("V16 ChargeMode: %d\n", m);
  sendModbusWriteSingleRegister(slaveID, reg_ForceChargeMode, (uint16_t)m);
}

// Fan master (manual override)
// Behavior: ON = force all fans ON; OFF = resume automatic control
BLYNK_WRITE(V17) {
  int s = param.asInt();
  Serial.printf("V17 Fan (manual): %d\n", s);
  if (s == 1) {
    manualOverride = true;
    fan1_state = fan2_state = fan3_state = HIGH;
    digitalWrite(R1_1, HIGH);
    digitalWrite(R1_2, HIGH);
    digitalWrite(R1_3, HIGH);
  } else {
    manualOverride = false;
    // automatic behavior will be applied at next scheduled read (or immediately if you call updateFansBasedOnTemps manually)
    Serial.println("Manual override cleared; resuming automatic control on next sensor update.");
  }
}

// Manual register inputs - ignore initial empty push from Blynk
BLYNK_WRITE(V20) {
  String tmp = param.asStr();
  if (tmp.length() == 0) return; // ignore early empty update
  regInput = tmp;
  manualReg = (uint16_t)regInput.toInt();
  Serial.printf("Manual Reg Input: %u\n", manualReg);
}
BLYNK_WRITE(V21) {
  valInput = param.asStr();
  // Accept integers only (strip possible decimals)
  manualVal = (int32_t)valInput.toInt();
  Serial.printf("Manual Val Input: %ld\n", (long)manualVal);
}

// Manual write
BLYNK_WRITE(V22) {
  if (param.asInt() == 1) {
    if (manualReg < 10000 || manualReg > 30000) {
      Serial.println("Manual WRITE rejected: register out of allowed range (10000..30000).");
      Blynk.virtualWrite(V24, "WRITE REJECT");
      return;
    }
    Serial.printf("Manual WRITE: reg=%u val=%ld\n", manualReg, (long)manualVal);
    sendModbusWriteSingleRegister(slaveID, manualReg, (uint16_t)manualVal);
    Blynk.virtualWrite(V24, "WRITE SENT");
  }
}

// Manual read
BLYNK_WRITE(V23) {
  if (param.asInt() == 1) {
    if (manualReg < 1 || manualReg > 65535) {
      Serial.println("Manual READ rejected: invalid register");
      Blynk.virtualWrite(V24, "READ REJECT");
      return;
    }
    float r = readModbusRegister(slaveID, manualReg);
    if (r < -1000.0f) {
      Blynk.virtualWrite(V24, "READ ERROR");
    } else {
      char buf[32];
      dtostrf(r, 0, 2, buf);
      Blynk.virtualWrite(V24, buf);
    }
  }
}

// =============================================================================
//                                MODBUS FUNCTIONS
// =============================================================================

// Helper: reinitialize RS485 interface (cheap recovery)
void rs485_reinit() {
  Serial.println("RS485: reinitializing HALFDUPLEX interface...");
  RS485.begin(9600, HALFDUPLEX);
  delay(RS485_REINIT_DELAY_MS);
  // flush any leftover bytes
  while (RS485.available()) { RS485.read(); }
  Serial.println("RS485: reinit done.");
  consecutiveReadFailures = 0;
}

// Modbus write with RS485 reinit after transmit (defensive)
void sendModbusWriteSingleRegister(byte id, uint16_t reg, uint16_t val) {
  byte frame[8];
  frame[0] = id;
  frame[1] = 0x06; // Write single
  frame[2] = highByte(reg);
  frame[3] = lowByte(reg);
  frame[4] = highByte(val);
  frame[5] = lowByte(val);

  uint16_t crc = calculateCRC(frame, 6);
  frame[6] = lowByte(crc);
  frame[7] = highByte(crc);

  // Transmit
  RS485.write(frame, 8);
  RS485.flush();

  // small settle time (allow driver to release DE)
  delay(40);

  // Defensive re-init to ensure DE/RE returned to RX
  rs485_reinit();
}

// Modbus read with retries & recovery on repeated failures
float readModbusRegister(byte id, uint16_t reg) {
  const int maxRetries = 3;
  const int readTimeoutMs = 120; // per attempt

  for (int attempt = 1; attempt <= maxRetries; ++attempt) {
    // build request
    byte request[8];
    request[0] = id;
    request[1] = 0x03; // Read Holding Registers
    request[2] = highByte(reg);
    request[3] = lowByte(reg);
    request[4] = 0x00;
    request[5] = 0x01;

    uint16_t crc = calculateCRC(request, 6);
    request[6] = lowByte(crc);
    request[7] = highByte(crc);

    // send request
    RS485.write(request, 8);
    RS485.flush();

    // read timed response
    byte response[64];
    int idx = 0;
    unsigned long start = millis();
    while (millis() - start < readTimeoutMs && idx < 7) {
      if (RS485.available()) {
        response[idx++] = RS485.read();
      }
    }

    if (idx >= 7 && response[1] == 0x03 && response[2] == 0x02) {
      uint16_t raw = (response[3] << 8) | response[4];
      float scaled = raw * scaleRegister(reg);
      // success -> reset failure counter
      consecutiveReadFailures = 0;
      return scaled;
    }

    // small backoff before retry
    delay(20);
  }

  // failed after retries
  Serial.printf("Modbus read failed reg=%u\n", reg);
  consecutiveReadFailures++;
  // if too many consecutive failures, attempt RS485 re-init
  if (consecutiveReadFailures >= RS485_RECOVER_THRESHOLD) {
    Serial.printf("ConsecutiveReadFailures=%d >= %d → attempting RS485 reinit\n", consecutiveReadFailures, RS485_RECOVER_THRESHOLD);
    rs485_reinit();
  }

  return -9999.0; // error sentinel
}

// -------------------------- Batch read helper (optional) -----------------
bool modbusReadMultiple(byte id, uint16_t startReg, uint16_t count, uint16_t *buffer) {
  if (count == 0 || count > 20) return false; // protect buffer size
  byte req[8];
  req[0] = id;
  req[1] = 0x03;
  req[2] = highByte(startReg);
  req[3] = lowByte(startReg);
  req[4] = highByte(count);
  req[5] = lowByte(count);
  uint16_t crc = calculateCRC(req, 6);
  req[6] = lowByte(crc);
  req[7] = highByte(crc);

  RS485.write(req, 8);
  RS485.flush();

  int expected = 5 + count * 2;
  byte resp[64];
  int idx = 0;
  unsigned long start = millis();
  while (millis() - start < 300 && idx < expected) {
    if (RS485.available()) resp[idx++] = RS485.read();
  }
  if (idx != expected) return false;

  for (int i = 0; i < count; ++i) {
    buffer[i] = (resp[3 + i * 2] << 8) | resp[4 + i * 2];
  }
  return true;
}

// -------------------------- Scaling lookup ------------------------------
float scaleRegister(uint16_t reg) {
  switch (reg) {
    case 15112:
    case 15113:
    case 15114:
      return 0.1f;

    case 15115:
      return 1.0f;

    case 25237:
    case 25240:
    case 25247:
    case 25250:
      return 0.1f;

    case 25243:
    case 25253:
      return 0.01f;

    case 15107:
      return 0.1f;
  }
  return 1.0f;
}

// -------------------------- CRC calc -----------------------------------
uint16_t calculateCRC(byte *array, byte len) {
  uint16_t crc = 0xFFFF;
  for (byte i = 0; i < len; i++) {
    crc ^= array[i];
    for (byte j = 0; j < 8; j++) {
      if (crc & 0x0001) {
        crc >>= 1;
        crc ^= 0xA001;
      } else crc >>= 1;
    }
  }
  return crc;
}

// -------------------------- Connectivity watchdog ----------------------
void checkConnections() {
  // WiFi auto-reconnect (non-blocking)
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastWiFiAttempt = 0;

    if (!reconnectInProgress && millis() - lastWiFiAttempt > 5000) {
      lastWiFiAttempt = millis();
      Serial.println("WiFi lost. Attempting reconnect...");
      if (!tryReconnectAnyNonBlocking() && !inConfigPortal) {
        wifiFailCount++;
        if (wifiFailCount >= WIFI_MAX_RETRIES) {
          Serial.println("WiFi reconnection failed repeatedly. Opening config portal...");
          startConfigPortal();
          wifiFailCount = 0;
        }
      }
      return;
    }

    if (reconnectInProgress) {
      if (!tryReconnectAnyNonBlocking() && !inConfigPortal) {
        wifiFailCount++;
        if (wifiFailCount >= WIFI_MAX_RETRIES) {
          Serial.println("WiFi reconnection failed repeatedly. Opening config portal...");
          startConfigPortal();
          wifiFailCount = 0;
        }
      }
    }
    return;
  }

  if (reconnectInProgress) {
    Serial.println("WiFi reconnected on slot " + String(reconnectCurrentIdx));
    lastGoodWiFiIndex = reconnectCurrentIdx;
    saveWiFiStore();
    initBlynkAndOta();
    resetReconnectState();
  }

  wifiFailCount = 0;

  // Blynk reconnect (non-blocking)
  if (!Blynk.connected() && WiFi.status() == WL_CONNECTED) {
    static unsigned long lastBlynkAttempt = 0;
    if (millis() - lastBlynkAttempt > 5000) {
      lastBlynkAttempt = millis();
      Serial.println("Blynk not connected. Reconnecting...");
      Blynk.connect();
    }
  }
}

// -------------------------- Fan control function -------------------------
void updateFansBasedOnTemps(float bmsTempC, float invTempC) {
  // If manual override is active, fans are already forced ON by V17 handler.
  if (manualOverride) {
    fan1_state = fan2_state = fan3_state = HIGH;
    digitalWrite(R1_1, HIGH);
    digitalWrite(R1_2, HIGH);
    digitalWrite(R1_3, HIGH);
    Blynk.virtualWrite(V17, 1);
    return;
  }

  // Emergency inverter override forces all fans ON
  if (invTempC >= INV_EMERG_ON) {
    fan1_state = fan2_state = fan3_state = HIGH;
  } else if (invTempC <= INV_EMERG_OFF) {
    // release emergency; proceed to battery control
    // (fall through to BMS logic)
  } else {
    // if inverter between OFF and ON emergency thresholds, do not force; fall through
  }

  // If not currently forced by inverter emergency, evaluate BMS-based logic
  if (!(invTempC >= INV_EMERG_ON)) {
    // Fan1 hysteresis
    if (bmsTempC >= FAN1_ON) fan1_state = HIGH;
    else if (bmsTempC <= FAN1_OFF) fan1_state = LOW;
    // Fan2 hysteresis
    if (bmsTempC >= FAN2_ON) fan2_state = HIGH;
    else if (bmsTempC <= FAN2_OFF) fan2_state = LOW;
    // Fan3 hysteresis
    if (bmsTempC >= FAN3_ON) fan3_state = HIGH;
    else if (bmsTempC <= FAN3_OFF) fan3_state = LOW;
  }

  // Apply outputs to relays
  digitalWrite(R1_1, fan1_state);
  digitalWrite(R1_2, fan2_state);
  digitalWrite(R1_3, fan3_state);

  // Sync Blynk switch: ON if any fan is ON
  int anyFan = (fan1_state == HIGH || fan2_state == HIGH || fan3_state == HIGH) ? 1 : 0;
  Blynk.virtualWrite(V17, anyFan);
}
