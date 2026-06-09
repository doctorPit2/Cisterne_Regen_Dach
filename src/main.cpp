#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <time.h>
#include <TFT_eSPI.h>
#include "esp_wifi_types.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

// WiFi-Konfiguration
const char* ssid = "lenovo";  // Dein WLAN-Name
const char* password = "lenovotablet";  // Dein WLAN-Passwort

// Feste IP-Konfiguration (anpassen falls gewünscht)
IPAddress local_IP(192, 168, 100, 150);  // Andere IP als Receiver
IPAddress gateway(192, 168, 100, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);
IPAddress secondaryDNS(8, 8, 4, 4);

// NTP-Server Konfiguration
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3600;        // GMT+1 für Deutschland
const int daylightOffset_sec = 3600;    // Sommerzeit

// TFT-Display
TFT_eSPI tft = TFT_eSPI();
#define TFT_BL 27  // Backlight Pin (anpassen falls anders)

// Webserver
AsyncWebServer server(80);

// Datenstruktur für Cisterne (muss identisch mit Sender sein!)
typedef struct {
  float waterLevel;                // Wasserstand in cm
  int adcValue;                    // Roher ADC-Wert
  bool pumpActive;                 // Status der Wasserpumpe
  bool pumpAlarm;                  // Pumpen-Alarm bei Laufzeitüberschreitung
  unsigned long pumpReferenceTime; // Referenzzeit in Sekunden
  unsigned long lastPumpDuration;  // Letzte Pumpdauer in Sekunden
} WaterLevelData;

// Datenstruktur für Wetterdaten vom Dach (muss identisch mit Sender sein!)
typedef struct {
  float STX;
  float Speed;
  float Dir;
  float Hum;
  float Taupunkt;
  float Temp;
  float Press;
  float sectic;
  float Regentic;
  float bmp085;
  float speedinv;
  float calcCheck;
  unsigned long timestamp;
} WetterDachData;

// Historische Daten für Chart
#define MAX_HISTORY 2000  // RAM-Buffer: Maximale Anzahl an Datenpunkten (bei 30-Min-Intervall = ~41 Tage)
#define MAX_ARCHIVE 10000  // Maximale Anzahl an archivierten Datenpunkten
#define ARCHIVE_BATCH 200  // Anzahl Punkte pro Archivierung
#define DATA_POINT_INTERVAL 1800  // Datenaufzeichnung alle 30 Minuten (1800 Sekunden)

struct DataPoint {
  unsigned long timestamp;  // Unix-Timestamp
  float waterLevel;         // Wasserstand in cm
  bool pumpActive;          // Pumpen-Status
};

DataPoint history[MAX_HISTORY];
int historyIndex = 0;
int historyCount = 0;

// Archiv-Daten (werden bei Bedarf aus LittleFS geladen)
struct ArchiveStats {
  int count;                // Anzahl archivierter Datenpunkte
  unsigned long oldestTime; // Ältester Timestamp im Archiv
  unsigned long newestTime; // Neuester Timestamp im Archiv
};

ArchiveStats archiveStats = {0, 0, 0};

// Pump-Event-Tracking
unsigned long lastPumpStartTime = 0;
unsigned long lastPumpEndTime = 0;
unsigned long currentPumpStartTime = 0;
unsigned long currentCycleStartTime = 0;  // Startzeit des aktuellen Füllzyklus
bool wasPumpActive = false;
unsigned long lastDataPointTime = 0;  // Zeitpunkt des letzten gespeicherten Datenpunkts

// Persistente Speicherung
unsigned long lastSaveTime = 0;
const unsigned long SAVE_INTERVAL = 300000;  // Alle 5 Minuten speichern (300.000 ms)
const char* HISTORY_FILE = "/history.json";
const char* ARCHIVE_FILE = "/history_archive.json";
const char* ARCHIVE_STATS_FILE = "/archive_stats.json";
const char* PUMP_FILE = "/pump_events.json";
const char* CISTERNE_FILE = "/cisterne_data.json";

WaterLevelData cisterne;
bool dataReceived = false;
unsigned long lastReceiveTime = 0;
String lastUpdateTime = "Warte auf Daten...";
bool displayInitialized = false;
int rssiValue = 0;  // RSSI-Wert in dBm

// Wetterdaten vom Dach
WetterDachData wetter_Dach;
bool wetterDataReceived = false;
uint8_t wetterSenderMAC[] = {0xd4, 0xe9, 0xf4, 0xe4, 0x2B, 0x04};  // MAC des Wetter-Senders

// Display-Modi
enum DisplayMode {
  DISPLAY_MODE_STATUS,   // Normale Status-Anzeige
  DISPLAY_MODE_CHART     // Chart-Anzeige
};

DisplayMode currentDisplayMode = DISPLAY_MODE_STATUS;
unsigned long lastModeSwitch = 0;
const unsigned long MODE_SWITCH_INTERVAL = 10000;  // Wechsel alle 10 Sekunden

// Farben
#define COLOR_BACKGROUND TFT_BLACK
#define COLOR_TEXT TFT_WHITE
#define COLOR_HEADER TFT_CYAN
#define COLOR_VALUE TFT_YELLOW
#define COLOR_ALARM TFT_RED
#define COLOR_OK TFT_GREEN
#define COLOR_RSSI_EXCELLENT TFT_GREEN
#define COLOR_RSSI_GOOD 0x07E0
#define COLOR_RSSI_FAIR TFT_ORANGE
#define COLOR_RSSI_WEAK TFT_RED

// ============================================================================
// LittleFS: Daten speichern und laden
// ============================================================================
void saveHistoryToFS() {
  Serial.println("Speichere Historie auf LittleFS...");
  
  File file = LittleFS.open(HISTORY_FILE, "w");
  if (!file) {
    Serial.println("Fehler beim Öffnen der History-Datei zum Schreiben");
    return;
  }
  
  // JSON-Dokument erstellen (großes Dokument für viele Datenpunkte)
  DynamicJsonDocument doc(MAX_HISTORY * 64 + 1024);
  JsonArray dataArray = doc.createNestedArray("data");
  
  // Alle Datenpunkte speichern
  int startIdx = (historyIndex - historyCount + MAX_HISTORY) % MAX_HISTORY;
  for (int i = 0; i < historyCount; i++) {
    int idx = (startIdx + i) % MAX_HISTORY;
    JsonObject point = dataArray.createNestedObject();
    point["ts"] = history[idx].timestamp;
    point["wl"] = history[idx].waterLevel;
    point["pa"] = history[idx].pumpActive;
  }
  
  doc["count"] = historyCount;
  doc["index"] = historyIndex;
  
  if (serializeJson(doc, file) == 0) {
    Serial.println("Fehler beim Schreiben der JSON-Daten");
  } else {
    Serial.printf("Historie gespeichert: %d Datenpunkte\n", historyCount);
  }
  
  file.close();
}

void loadHistoryFromFS() {
  Serial.println("Lade Historie von LittleFS...");
  
  if (!LittleFS.exists(HISTORY_FILE)) {
    Serial.println("Keine gespeicherte Historie gefunden");
    return;
  }
  
  File file = LittleFS.open(HISTORY_FILE, "r");
  if (!file) {
    Serial.println("Fehler beim Öffnen der History-Datei zum Lesen");
    return;
  }
  
  DynamicJsonDocument doc(MAX_HISTORY * 64 + 1024);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) {
    Serial.print("Fehler beim Parsen der JSON-Daten: ");
    Serial.println(error.c_str());
    return;
  }
  
  // Historie laden (X-Achse skaliert jetzt korrekt von 0 bis JETZT)
  historyCount = doc["count"] | 0;
  historyIndex = doc["index"] | 0;
  
  JsonArray dataArray = doc["data"];
  int loadedPoints = 0;
  
  for (JsonObject point : dataArray) {
    if (loadedPoints < MAX_HISTORY) {
      history[loadedPoints].timestamp = point["ts"] | 0;
      history[loadedPoints].waterLevel = point["wl"] | 0.0;
      history[loadedPoints].pumpActive = point["pa"] | false;
      loadedPoints++;
    }
  }
  
  Serial.printf("Historie geladen: %d Datenpunkte wiederhergestellt\n", historyCount);
}

void savePumpEventsToFS() {
  Serial.println("Speichere Pump-Events auf LittleFS...");
  
  File file = LittleFS.open(PUMP_FILE, "w");
  if (!file) {
    Serial.println("Fehler beim Öffnen der Pump-Events-Datei");
    return;
  }
  
  DynamicJsonDocument doc(512);
  doc["lastPumpStartTime"] = lastPumpStartTime;
  doc["lastPumpEndTime"] = lastPumpEndTime;
  doc["currentPumpStartTime"] = currentPumpStartTime;
  doc["currentCycleStartTime"] = currentCycleStartTime;
  doc["wasPumpActive"] = wasPumpActive;
  doc["lastDataPointTime"] = lastDataPointTime;
  
  if (serializeJson(doc, file) == 0) {
    Serial.println("Fehler beim Schreiben der Pump-Events");
  } else {
    Serial.println("Pump-Events gespeichert");
  }
  
  file.close();
}

void loadPumpEventsFromFS() {
  Serial.println("Lade Pump-Events von LittleFS...");
  
  if (!LittleFS.exists(PUMP_FILE)) {
    Serial.println("Keine gespeicherten Pump-Events gefunden");
    return;
  }
  
  File file = LittleFS.open(PUMP_FILE, "r");
  if (!file) {
    Serial.println("Fehler beim Öffnen der Pump-Events-Datei");
    return;
  }
  
  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) {
    Serial.print("Fehler beim Parsen der Pump-Events: ");
    Serial.println(error.c_str());
    return;
  }
  
  lastPumpStartTime = doc["lastPumpStartTime"] | 0;
  lastPumpEndTime = doc["lastPumpEndTime"] | 0;
  currentPumpStartTime = doc["currentPumpStartTime"] | 0;
  currentCycleStartTime = doc["currentCycleStartTime"] | 0;  // Jetzt DOCH laden - X-Achse skaliert korrekt!
  wasPumpActive = doc["wasPumpActive"] | false;
  lastDataPointTime = doc["lastDataPointTime"] | 0;
  
  Serial.println("Pump-Events geladen");
  Serial.printf("  Letzter Pumpstart: %lu\n", lastPumpStartTime);
  Serial.printf("  Aktueller Pumpstart: %lu\n", currentPumpStartTime);
  Serial.printf("  Zyklusstartzeit: %lu\n", currentCycleStartTime);
}

void saveCisterneDataToFS() {
  Serial.println("Speichere Cisterne-Daten auf LittleFS...");
  
  File file = LittleFS.open(CISTERNE_FILE, "w");
  if (!file) {
    Serial.println("Fehler beim Öffnen der Cisterne-Daten-Datei");
    return;
  }
  
  DynamicJsonDocument doc(512);
  doc["waterLevel"] = cisterne.waterLevel;
  doc["adcValue"] = cisterne.adcValue;
  doc["pumpActive"] = cisterne.pumpActive;
  doc["pumpAlarm"] = cisterne.pumpAlarm;
  doc["pumpReferenceTime"] = cisterne.pumpReferenceTime;
  doc["lastPumpDuration"] = cisterne.lastPumpDuration;
  doc["dataReceived"] = dataReceived;
  doc["lastReceiveTime"] = lastReceiveTime;
  doc["lastUpdateTime"] = lastUpdateTime;
  doc["rssiValue"] = rssiValue;
  
  if (serializeJson(doc, file) == 0) {
    Serial.println("Fehler beim Schreiben der Cisterne-Daten");
  } else {
    Serial.println("Cisterne-Daten gespeichert");
  }
  
  file.close();
}

void loadCisterneDataFromFS() {
  Serial.println("Lade Cisterne-Daten von LittleFS...");
  
  if (!LittleFS.exists(CISTERNE_FILE)) {
    Serial.println("Keine gespeicherten Cisterne-Daten gefunden");
    return;
  }
  
  File file = LittleFS.open(CISTERNE_FILE, "r");
  if (!file) {
    Serial.println("Fehler beim Öffnen der Cisterne-Daten-Datei");
    return;
  }
  
  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) {
    Serial.print("Fehler beim Parsen der Cisterne-Daten: ");
    Serial.println(error.c_str());
    return;
  }
  
  cisterne.waterLevel = doc["waterLevel"] | 0.0f;
  cisterne.adcValue = doc["adcValue"] | 0;
  cisterne.pumpActive = doc["pumpActive"] | false;
  cisterne.pumpAlarm = doc["pumpAlarm"] | false;
  cisterne.pumpReferenceTime = doc["pumpReferenceTime"] | 0;
  cisterne.lastPumpDuration = doc["lastPumpDuration"] | 0;
  dataReceived = doc["dataReceived"] | false;
  lastReceiveTime = doc["lastReceiveTime"] | 0;
  lastUpdateTime = doc["lastUpdateTime"] | String("Warte auf Daten...");
  rssiValue = doc["rssiValue"] | 0;
  
  Serial.println("Cisterne-Daten geladen");
  Serial.printf("  Wasserstand: %.1f cm\n", cisterne.waterLevel);
  Serial.printf("  Pumpe aktiv: %s\n", cisterne.pumpActive ? "JA" : "NEIN");
  Serial.printf("  Letzte Pumpdauer: %lu s\n", cisterne.lastPumpDuration);
}

// ============================================================================
// Archivierungs-Funktionen
// ============================================================================

// Archiv-Statistiken laden
void loadArchiveStats() {
  if (!LittleFS.exists(ARCHIVE_STATS_FILE)) {
    archiveStats.count = 0;
    archiveStats.oldestTime = 0;
    archiveStats.newestTime = 0;
    return;
  }
  
  File file = LittleFS.open(ARCHIVE_STATS_FILE, "r");
  if (!file) return;
  
  DynamicJsonDocument doc(256);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (!error) {
    archiveStats.count = doc["count"] | 0;
    archiveStats.oldestTime = doc["oldest"] | 0;
    archiveStats.newestTime = doc["newest"] | 0;
    Serial.printf("Archiv-Stats geladen: %d Punkte (%lu - %lu)\n", 
                  archiveStats.count, archiveStats.oldestTime, archiveStats.newestTime);
  }
}

// Archiv-Statistiken speichern
void saveArchiveStats() {
  File file = LittleFS.open(ARCHIVE_STATS_FILE, "w");
  if (!file) return;
  
  DynamicJsonDocument doc(256);
  doc["count"] = archiveStats.count;
  doc["oldest"] = archiveStats.oldestTime;
  doc["newest"] = archiveStats.newestTime;
  
  serializeJson(doc, file);
  file.close();
}

// Älteste Datenpunkte aus RAM ins Archiv verschieben
void archiveOldData() {
  if (historyCount < MAX_HISTORY) return;  // Buffer noch nicht voll
  
  Serial.printf("Archiviere %d älteste Datenpunkte...\n", ARCHIVE_BATCH);
  
  // Bestehende Archiv-Daten laden
  DynamicJsonDocument archiveDoc(MAX_ARCHIVE * 64 + 2048);
  JsonArray archiveArray;
  
  if (LittleFS.exists(ARCHIVE_FILE)) {
    File file = LittleFS.open(ARCHIVE_FILE, "r");
    if (file) {
      DeserializationError error = deserializeJson(archiveDoc, file);
      file.close();
      if (!error) {
        archiveArray = archiveDoc["data"].as<JsonArray>();
      }
    }
  }
  
  if (archiveArray.isNull()) {
    archiveArray = archiveDoc.createNestedArray("data");
  }
  
  // Älteste Punkte aus RAM hinzufügen
  int startIdx = (historyIndex - historyCount + MAX_HISTORY) % MAX_HISTORY;
  for (int i = 0; i < ARCHIVE_BATCH && i < historyCount; i++) {
    int idx = (startIdx + i) % MAX_HISTORY;
    
    JsonObject point = archiveArray.createNestedObject();
    point["ts"] = history[idx].timestamp;
    point["wl"] = history[idx].waterLevel;
    point["pa"] = history[idx].pumpActive;
    
    // Stats aktualisieren
    if (archiveStats.count == 0 || history[idx].timestamp < archiveStats.oldestTime) {
      archiveStats.oldestTime = history[idx].timestamp;
    }
    if (history[idx].timestamp > archiveStats.newestTime) {
      archiveStats.newestTime = history[idx].timestamp;
    }
  }
  
  archiveStats.count = archiveArray.size();
  
  // Wenn Archiv zu groß wird, älteste Einträge entfernen
  while (archiveStats.count > MAX_ARCHIVE) {
    archiveArray.remove(0);
    archiveStats.count--;
    // Neue älteste Zeit finden
    if (archiveStats.count > 0) {
      archiveStats.oldestTime = archiveArray[0]["ts"];
    }
  }
  
  // Archiv speichern
  File file = LittleFS.open(ARCHIVE_FILE, "w");
  if (file) {
    serializeJson(archiveDoc, file);
    file.close();
    saveArchiveStats();
    Serial.printf("Archiv gespeichert: %d Punkte\n", archiveStats.count);
  }
  
  // Archivierte Punkte aus RAM entfernen
  // Verschiebe verbleibende Punkte nach vorne
  int newCount = historyCount - ARCHIVE_BATCH;
  for (int i = 0; i < newCount; i++) {
    int srcIdx = (startIdx + ARCHIVE_BATCH + i) % MAX_HISTORY;
    history[i] = history[srcIdx];
  }
  historyIndex = newCount;
  historyCount = newCount;
  
  Serial.printf("RAM-Buffer bereinigt: %d Punkte verbleiben\n", historyCount);
}

// Archiv für neuen Zyklus löschen
void clearArchiveForNewCycle() {
  Serial.println("Lösche Archiv für neuen Zyklus...");
  
  if (LittleFS.exists(ARCHIVE_FILE)) {
    LittleFS.remove(ARCHIVE_FILE);
  }
  if (LittleFS.exists(ARCHIVE_STATS_FILE)) {
    LittleFS.remove(ARCHIVE_STATS_FILE);
  }
  
  archiveStats.count = 0;
  archiveStats.oldestTime = 0;
  archiveStats.newestTime = 0;
  
  Serial.println("Archiv gelöscht");
}

void saveAllData() {
  saveHistoryToFS();
  savePumpEventsToFS();
  saveCisterneDataToFS();
}

void loadAllData() {
  // Historie und alle Events laden - Chart-Daten bleiben nach Reset erhalten!
  loadHistoryFromFS();  // Jetzt aktiviert - X-Achse skaliert korrekt von 0 bis JETZT
  loadArchiveStats();   // Archiv-Statistiken laden
  loadPumpEventsFromFS();
  loadCisterneDataFromFS();
  
  Serial.println("Alle Daten geladen - Chart-Historie nach Reset wiederhergestellt");
}

// ============================================================================
// Historische Daten speichern
// ============================================================================
void addDataPoint(float waterLevel, bool pumpActive) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return;  // Keine gültige Zeit
  }
  
  time_t now = mktime(&timeinfo);
  
  history[historyIndex].timestamp = now;
  history[historyIndex].waterLevel = waterLevel;
  history[historyIndex].pumpActive = pumpActive;
  
  historyIndex = (historyIndex + 1) % MAX_HISTORY;
  if (historyCount < MAX_HISTORY) {
    historyCount++;
  } else {
    // Buffer ist voll - archiviere älteste Daten
    archiveOldData();
  }
}

// Pump-Events tracken
void trackPumpEvents() {
  bool saveNeeded = false;
  
  // Neuer Zyklus erkannt (Pumpe startet nach längerer Pause)
  if (cisterne.pumpActive && !wasPumpActive) {
    // Prüfe ob letzter Pumpvorgang lange her ist (> 5 Minuten)
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      time_t now = mktime(&timeinfo);
      if (currentCycleStartTime == 0 || (now - lastPumpEndTime) > 300) {
        // Neuer Zyklus - nur Startzeit setzen, Historie für Monatsübersicht beibehalten
        Serial.println("Neuer Füllzyklus erkannt - Zyklusstartzeit wird aktualisiert (Historie bleibt erhalten)");
        // clearArchiveForNewCycle();  // DEAKTIVIERT - Archiv für Monatsübersicht behalten
        // historyCount = 0;  // DEAKTIVIERT - Historie für Monatsübersicht behalten
        // historyIndex = 0;  // DEAKTIVIERT - Historie für Monatsübersicht behalten
        currentCycleStartTime = now;
      }
    }
  }
  
  // Pump wurde aktiviert (Füllphase endet, Pumpphase beginnt)
  if (cisterne.pumpActive && !wasPumpActive) {
    lastPumpStartTime = currentPumpStartTime;
    lastPumpEndTime = currentPumpStartTime;  // Ende = Start des vorherigen
    currentPumpStartTime = time(nullptr);
    
    Serial.printf("Pump gestartet um: %lu (Füllzyklus endet, Pumpphase beginnt)\n", currentPumpStartTime);
    saveNeeded = true;  // Wichtiges Event - sofort speichern
  }
  // Pump wurde deaktiviert (Pumpphase endet, neuer Füllzyklus beginnt)
  else if (!cisterne.pumpActive && wasPumpActive) {
    unsigned long now = time(nullptr);
    currentCycleStartTime = now;  // Neuer Füllzyklus beginnt
    
    // WICHTIG: Historie NICHT löschen - wird für Monatsübersicht benötigt!
    // Der Chart des aktuellen Zyklus filtert automatisch ab currentCycleStartTime
    
    Serial.printf("Pump gestoppt um: %lu - Neuer Füllzyklus beginnt (Historie bleibt für Monatsübersicht erhalten)\n", now);
    saveNeeded = true;  // Wichtiges Event - sofort speichern
  }
  
  wasPumpActive = cisterne.pumpActive;
  
  // Bei wichtigen Events sofort speichern
  if (saveNeeded) {
    saveAllData();
  }
}

// Zeit zwischen Pumpvorgängen formatieren
String formatTimeDifference(unsigned long seconds) {
  if (seconds == 0) return "Keine Daten";
  
  int days = seconds / 86400;
  int hours = (seconds % 86400) / 3600;
  int minutes = (seconds % 3600) / 60;
  int secs = seconds % 60;
  
  String result = "";
  if (days > 0) result += String(days) + "T ";
  if (hours > 0 || days > 0) result += String(hours) + "h ";
  if (minutes > 0 || hours > 0 || days > 0) result += String(minutes) + "m ";
  result += String(secs) + "s";
  
  return result;
}

// ============================================================================
// WIFI PROMISCUOUS CALLBACK für RSSI-Erfassung
// ============================================================================
void promiscuous_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
  if (type == WIFI_PKT_MGMT) return;  // Nur Daten-Pakete interessieren uns
  
  wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
  wifi_pkt_rx_ctrl_t *ctrl = &pkt->rx_ctrl;
  
  // RSSI speichern (wird bei jedem Paket aktualisiert)
  rssiValue = ctrl->rssi;
}

// ESP-NOW Callback wenn Daten empfangen werden
void OnDataRecv(const uint8_t *mac_addr, const uint8_t *data, int data_len) {
  // MAC-Adresse des Senders anzeigen
  Serial.println("==================================================");
  Serial.println("ESP-NOW Daten empfangen von Sender:");
  Serial.printf("  MAC-Adresse: %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac_addr[0], mac_addr[1], mac_addr[2], 
                mac_addr[3], mac_addr[4], mac_addr[5]);
  Serial.printf("  Datenlänge: %d Bytes\n", data_len);
  Serial.println("==================================================");
  
  // Prüfe, ob es Wetterdaten vom Dach sind
  bool isWetterSender = true;
  for (int i = 0; i < 6; i++) {
    if (mac_addr[i] != wetterSenderMAC[i]) {
      isWetterSender = false;
      break;
    }
  }
  
  if (isWetterSender && data_len == sizeof(WetterDachData)) {
    // Wetterdaten empfangen
    memcpy(&wetter_Dach, data, sizeof(WetterDachData));
    wetterDataReceived = true;
    
    Serial.println(">>> WETTERDATEN VOM DACH <<<");
    Serial.printf("  STX:          %.2f\n", wetter_Dach.STX);
    Serial.printf("  Speed:        %.2f\n", wetter_Dach.Speed);
    Serial.printf("  Dir:          %.2f\n", wetter_Dach.Dir);
    Serial.printf("  Hum:          %.2f %%\n", wetter_Dach.Hum);
    Serial.printf("  Taupunkt:     %.2f °C\n", wetter_Dach.Taupunkt);
    Serial.printf("  Temp:         %.2f °C\n", wetter_Dach.Temp);
    Serial.printf("  Press:        %.2f hPa\n", wetter_Dach.Press);
    Serial.printf("  sectic:       %.2f\n", wetter_Dach.sectic);
    Serial.printf("  Regentic:     %.2f\n", wetter_Dach.Regentic);
    Serial.printf("  bmp085:       %.2f\n", wetter_Dach.bmp085);
    Serial.printf("  speedinv:     %.2f\n", wetter_Dach.speedinv);
    Serial.printf("  calcCheck:    %.2f\n", wetter_Dach.calcCheck);
    Serial.printf("  Timestamp:    %lu\n", wetter_Dach.timestamp);
    Serial.printf("  RSSI:         %d dBm\n", rssiValue);
    Serial.println("==================================================");
    
  } else if (data_len == sizeof(WaterLevelData)) {
    // Cisternendaten empfangen
    memcpy(&cisterne, data, sizeof(WaterLevelData));
    dataReceived = true;
    lastReceiveTime = millis();
    
    // RSSI wurde bereits im Promiscuous Callback erfasst
    
    // Aktuelle Zeit holen
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      char timeStr[64];
      strftime(timeStr, sizeof(timeStr), "%d.%m.%Y %H:%M:%S", &timeinfo);
      lastUpdateTime = String(timeStr);
    }
    
    // Historische Daten speichern (nur alle 30 Minuten, außer bei Pumpen-Statuswechsel)
    time_t now = time(nullptr);
    bool pumpStatusChanged = (cisterne.pumpActive != wasPumpActive);
    bool intervalElapsed = (now - lastDataPointTime) >= DATA_POINT_INTERVAL;
    
    if (pumpStatusChanged || intervalElapsed || lastDataPointTime == 0) {
      addDataPoint(cisterne.waterLevel, cisterne.pumpActive);
      lastDataPointTime = now;
      Serial.printf("Datenpunkt gespeichert (Grund: %s)\n", 
        pumpStatusChanged ? "Pumpen-Status" : "5-Min-Intervall");
    }
    
    // Pump-Events tracken
    trackPumpEvents();
    
    Serial.println("Cisternendaten empfangen:");
    Serial.printf("  Wasserstand: %.1f cm\n", cisterne.waterLevel);
    Serial.printf("  ADC-Wert: %d\n", cisterne.adcValue);
    Serial.printf("  Pumpe aktiv: %s\n", cisterne.pumpActive ? "JA" : "NEIN");
    Serial.printf("  Pumpen-Alarm: %s\n", cisterne.pumpAlarm ? "JA" : "NEIN");
    Serial.printf("  Letzte Pumpdauer: %lu s\n", cisterne.lastPumpDuration);
    Serial.printf("  RSSI: %d dBm\n", rssiValue);
  } else {
    Serial.println("WARNUNG: Datenlänge stimmt nicht überein!");
    Serial.printf("  Erwartet für Wetter: %d Bytes\n", sizeof(WetterDachData));
    Serial.printf("  Erwartet für Cisterne: %d Bytes\n", sizeof(WaterLevelData));
    Serial.printf("  Erhalten: %d Bytes\n", data_len);
  }
}

// RSSI-Balkendiagramm zeichnen
void drawRSSIBars(int x, int y, int rssi) {
  // RSSI-Label
  tft.setTextSize(1);
  tft.setTextColor(COLOR_TEXT, COLOR_BACKGROUND);
  tft.setCursor(x, y);
  tft.print("Signal:");
  
  // RSSI-Wert anzeigen
  tft.setCursor(x + 42, y);
  tft.printf("%d dBm", rssi);
  
  // Balken zeichnen (5 Balken)
  int barX = x;
  int barY = y + 12;
  int barWidth = 8;
  int barSpacing = 2;
  
  // Signalstärke bestimmen (-30 = exzellent, -90 = sehr schwach)
  int bars = 0;
  uint16_t barColor = COLOR_RSSI_WEAK;
  
  if (rssi >= -50) {
    bars = 5;
    barColor = COLOR_RSSI_EXCELLENT;
  } else if (rssi >= -60) {
    bars = 4;
    barColor = COLOR_RSSI_GOOD;
  } else if (rssi >= -70) {
    bars = 3;
    barColor = COLOR_RSSI_FAIR;
  } else if (rssi >= -80) {
    bars = 2;
    barColor = COLOR_RSSI_WEAK;
  } else {
    bars = 1;
    barColor = COLOR_RSSI_WEAK;
  }
  
  // Balken zeichnen
  for (int i = 0; i < 5; i++) {
    int height = 4 + (i * 3);  // Steigende Höhe
    uint16_t color = (i < bars) ? barColor : TFT_DARKGREY;
    tft.fillRect(barX + i * (barWidth + barSpacing), barY - height, barWidth, height, color);
  }
}

// TFT-Display initialisieren (einmalig - statische Elemente)
void initDisplayLayout() {
  tft.fillScreen(COLOR_BACKGROUND);
  
  // Header
  tft.setTextColor(COLOR_HEADER, COLOR_BACKGROUND);
  tft.setTextSize(2);
  tft.setCursor(10, 40);
  tft.println("CISTERNE Monitor");
  
  // Trennlinie
  tft.drawLine(0, 65, 480, 65, COLOR_HEADER);
  
  // Wasserstand Label (groß und zentriert)
  tft.setTextColor(COLOR_TEXT, COLOR_BACKGROUND);
  tft.setTextSize(2);
  int centerX = 240;
  String wsLabel = "Wasserstand";
  int labelWidth = wsLabel.length() * 12;
  tft.setCursor(centerX - labelWidth/2, 85);
  tft.println(wsLabel);
  
  // Untere Labels
  int col1X = 10;
  int col2X = 250;
  int row1Y = 180;
  int row2Y = 230;
  
  tft.setTextSize(1);
  tft.setTextColor(COLOR_TEXT, COLOR_BACKGROUND);
  tft.setCursor(col1X, row1Y);
  tft.println("ADC-Wert:");
  
  tft.setCursor(col2X, row1Y);
  tft.println("Pumpe:");
  
  tft.setCursor(col1X, row2Y);
  tft.println("Letzte Pumpdauer:");
  
  displayInitialized = true;
}

// TFT-Display aktualisieren (nur dynamische Werte)
void updateDisplay() {
  if (!displayInitialized) {
    initDisplayLayout();
  }
  
  // Uhrzeit oben (größer) - Bereich löschen und neu schreiben
  tft.fillRect(10, 10, 300, 20, COLOR_BACKGROUND);
  tft.setTextColor(COLOR_VALUE, COLOR_BACKGROUND);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println(lastUpdateTime);
  
  // RSSI-Anzeige oben rechts - Bereich löschen und neu zeichnen
  tft.fillRect(320, 10, 150, 25, COLOR_BACKGROUND);
  if (dataReceived) {
    drawRSSIBars(320, 10, rssiValue);
  }
  
  if (!dataReceived) {
    tft.fillRect(10, 100, 460, 40, COLOR_BACKGROUND);
    tft.setTextColor(COLOR_TEXT, COLOR_BACKGROUND);
    tft.setTextSize(2);
    tft.setCursor(10, 100);
    tft.println("Warte auf");
    tft.setCursor(10, 120);
    tft.println("Daten...");
    return;
  }
  
  // Wasserstand Wert (groß und zentriert) - Bereich löschen
  tft.fillRect(0, 115, 480, 50, COLOR_BACKGROUND);
  tft.setTextSize(4);
  tft.setTextColor(COLOR_VALUE, COLOR_BACKGROUND);
  char wsValue[20];
  sprintf(wsValue, "%.1f cm", cisterne.waterLevel);
  int centerX = 240;
  int valueWidth = strlen(wsValue) * 24;
  tft.setCursor(centerX - valueWidth/2, 115);
  tft.println(wsValue);
  
  // Koordinaten
  int col1X = 10;
  int col2X = 250;
  int row1Y = 180;
  int row2Y = 230;
  
  // ADC-Wert - Bereich löschen und neu schreiben
  tft.fillRect(col1X, row1Y + 15, 220, 20, COLOR_BACKGROUND);
  tft.setTextSize(2);
  tft.setTextColor(COLOR_VALUE, COLOR_BACKGROUND);
  tft.setCursor(col1X, row1Y + 15);
  tft.printf("%d", cisterne.adcValue);
  
  // Pumpen-Status - Bereich löschen und neu schreiben
  tft.fillRect(col2X, row1Y + 15, 220, 20, COLOR_BACKGROUND);
  tft.setTextSize(2);
  if (cisterne.pumpActive) {
    tft.setTextColor(COLOR_OK, COLOR_BACKGROUND);
    tft.setCursor(col2X, row1Y + 15);
    tft.println("AKTIV");
  } else {
    tft.setTextColor(COLOR_TEXT, COLOR_BACKGROUND);
    tft.setCursor(col2X, row1Y + 15);
    tft.println("AUS  ");
  }
  
  // Letzte Pumpdauer - Bereich löschen und neu schreiben
  tft.fillRect(col1X, row2Y + 15, 220, 20, COLOR_BACKGROUND);
  tft.setTextSize(2);
  tft.setTextColor(COLOR_VALUE, COLOR_BACKGROUND);
  tft.setCursor(col1X, row2Y + 15);
  tft.printf("%lu s", cisterne.lastPumpDuration);
  
  // Pumpen-Alarm - Bereich löschen und neu schreiben
  tft.fillRect(col2X, row2Y + 10, 220, 20, COLOR_BACKGROUND);
  if (cisterne.pumpAlarm) {
    tft.setTextSize(2);
    tft.setTextColor(COLOR_ALARM, COLOR_BACKGROUND);
    tft.setCursor(col2X, row2Y + 10);
    tft.println("! ALARM !");
  }
  
  // Datenalter unten - Bereich löschen und neu schreiben
  tft.fillRect(10, 300, 200, 10, COLOR_BACKGROUND);
  unsigned long dataAge = (millis() - lastReceiveTime) / 1000;
  tft.setTextSize(1);
  tft.setTextColor(COLOR_TEXT, COLOR_BACKGROUND);
  tft.setCursor(10, 300);
  tft.printf("Daten: vor %lu s", dataAge);
}

// ============================================================================
// Chart-Hilfsfunktionen (mit Archiv-Unterstützung)
// ============================================================================

// Struktur für kombinierten Datenzugriff (Archiv + RAM)
struct CombinedDataIterator {
  int archiveCount;
  int ramStartIdx;
  int totalCount;
  DynamicJsonDocument* archiveDoc;
  JsonArray archiveArray;
  
  CombinedDataIterator() : archiveDoc(nullptr), archiveCount(0), totalCount(0) {}
  
  ~CombinedDataIterator() {
    if (archiveDoc) {
      delete archiveDoc;
    }
  }
  
  // Initialisieren und Archiv laden
  bool init() {
    archiveCount = 0;
    ramStartIdx = (historyIndex - historyCount + MAX_HISTORY) % MAX_HISTORY;
    totalCount = historyCount;
    
    // Archiv laden falls vorhanden
    if (LittleFS.exists(ARCHIVE_FILE)) {
      archiveDoc = new DynamicJsonDocument(MAX_ARCHIVE * 64 + 2048);
      File file = LittleFS.open(ARCHIVE_FILE, "r");
      if (file) {
        DeserializationError error = deserializeJson(*archiveDoc, file);
        file.close();
        if (!error) {
          archiveArray = (*archiveDoc)["data"].as<JsonArray>();
          archiveCount = archiveArray.size();
          totalCount += archiveCount;
          Serial.printf("Chart-Iterator: %d Archiv + %d RAM = %d gesamt\n", 
                        archiveCount, historyCount, totalCount);
        }
      }
    }
    
    return totalCount > 0;
  }
  
  // Datenpunkt an Index i holen (0 = ältester)
  DataPoint getPoint(int i) {
    DataPoint point;
    
    if (i < archiveCount) {
      // Aus Archiv
      point.timestamp = archiveArray[i]["ts"];
      point.waterLevel = archiveArray[i]["wl"];
      point.pumpActive = archiveArray[i]["pa"];
    } else {
      // Aus RAM
      int ramIdx = i - archiveCount;
      int idx = (ramStartIdx + ramIdx) % MAX_HISTORY;
      point = history[idx];
    }
    
    return point;
  }
};

// Intelligentes Daten-Sampling: Reduziert Datenpunkte auf maxPoints
void sampleData(CombinedDataIterator& dataIter, DataPoint* sampledData, int& sampledCount, int maxPoints) {
  sampledCount = 0;
  
  if (dataIter.totalCount == 0 || currentCycleStartTime == 0) return;
  
  // Nur gültige Punkte zählen (ab Zyklusstart)
  int validCount = 0;
  for (int i = 0; i < dataIter.totalCount; i++) {
    DataPoint pt = dataIter.getPoint(i);
    if (pt.timestamp >= currentCycleStartTime) {
      validCount++;
    }
  }
  
  if (validCount <= maxPoints) {
    // Alle Punkte verwenden
    for (int i = 0; i < dataIter.totalCount; i++) {
      DataPoint pt = dataIter.getPoint(i);
      if (pt.timestamp >= currentCycleStartTime) {
        sampledData[sampledCount++] = pt;
      }
    }
  } else {
    // Intelligentes Sampling: nehme jeden N-ten Punkt
    float step = (float)validCount / maxPoints;
    int validIdx = 0;
    
    for (int i = 0; i < dataIter.totalCount; i++) {
      DataPoint pt = dataIter.getPoint(i);
      if (pt.timestamp >= currentCycleStartTime) {
        // Nehme diesen Punkt wenn er auf dem Sample-Grid liegt
        if (validIdx == 0 || validIdx >= (int)(sampledCount * step)) {
          sampledData[sampledCount++] = pt;
          if (sampledCount >= maxPoints) break;
        }
        validIdx++;
      }
    }
  }
}

// TFT Chart zeichnen (OPTIMIERT mit Sampling)
void drawChart() {
  tft.fillScreen(COLOR_BACKGROUND);
  
  // Header mit Startzeit des aktuellen Füllzyklus
  tft.setTextColor(COLOR_HEADER, COLOR_BACKGROUND);
  tft.setTextSize(2);
  tft.setCursor(10, 5);
  tft.print("Fuellzyklus Start: ");
  
  // Startzeit formatieren
  if (currentCycleStartTime > 0) {
    struct tm timeinfo;
    time_t startTime = (time_t)currentCycleStartTime;
    localtime_r(&startTime, &timeinfo);
    char startTimeStr[20];
    sprintf(startTimeStr, "%02d.%02d %02d:%02d", 
            timeinfo.tm_mday, timeinfo.tm_mon + 1,
            timeinfo.tm_hour, timeinfo.tm_min);
    tft.setTextColor(COLOR_VALUE, COLOR_BACKGROUND);
    tft.print(startTimeStr);
  }
  
  // Chart-Bereich definieren
  int chartX = 60;        // Links-Rand für Y-Achse
  int chartY = 35;        // Oben-Rand
  int chartWidth = 400;   // Breite des Charts
  int chartHeight = 200;  // Höhe des Charts (reduziert für X-Achsen-Beschriftung)
  
  if (currentCycleStartTime == 0) {
    tft.setTextSize(2);
    tft.setTextColor(COLOR_TEXT, COLOR_BACKGROUND);
    tft.setCursor(120, 150);
    tft.println("Warte auf Zyklus");
    return;
  }
  
  // OPTIMIERT: Nur RAM-Daten verwenden (KEIN Archiv!)
  // Sammle gültige Punkte aus RAM
  int ramStartIdx = (historyIndex - historyCount + MAX_HISTORY) % MAX_HISTORY;
  int validCount = 0;
  
  for (int i = 0; i < historyCount; i++) {
    int idx = (ramStartIdx + i) % MAX_HISTORY;
    if (history[idx].timestamp >= currentCycleStartTime) {
      validCount++;
    }
  }
  
  if (validCount < 2) {
    tft.setTextSize(2);
    tft.setTextColor(COLOR_TEXT, COLOR_BACKGROUND);
    tft.setCursor(80, 150);
    tft.println("Warte auf Daten...");
    return;
  }
  
  // Sampling: Max 100 Punkte für TFT (noch weniger für schnelleres Zeichnen)
  const int MAX_DISPLAY_POINTS = 100;
  DataPoint* sampledData = new DataPoint[MAX_DISPLAY_POINTS];
  int sampledCount = 0;
  
  // Intelligentes Sampling
  if (validCount <= MAX_DISPLAY_POINTS) {
    // Alle Punkte verwenden
    for (int i = 0; i < historyCount; i++) {
      int idx = (ramStartIdx + i) % MAX_HISTORY;
      if (history[idx].timestamp >= currentCycleStartTime) {
        sampledData[sampledCount++] = history[idx];
      }
    }
  } else {
    // Sampling: Jeden N-ten Punkt
    float step = (float)validCount / MAX_DISPLAY_POINTS;
    int validIdx = 0;
    
    for (int i = 0; i < historyCount && sampledCount < MAX_DISPLAY_POINTS; i++) {
      int idx = (ramStartIdx + i) % MAX_HISTORY;
      if (history[idx].timestamp >= currentCycleStartTime) {
        if (validIdx == 0 || validIdx >= (int)(sampledCount * step)) {
          sampledData[sampledCount++] = history[idx];
        }
        validIdx++;
      }
    }
  }
  
  // Min/Max für automatische Skalierung
  float minWaterLevel = 999.0;
  float maxWaterLevel = 0.0;
  
  for (int i = 0; i < sampledCount; i++) {
    if (sampledData[i].waterLevel < minWaterLevel) minWaterLevel = sampledData[i].waterLevel;
    if (sampledData[i].waterLevel > maxWaterLevel) maxWaterLevel = sampledData[i].waterLevel;
  }
  
  // Y-Achsen-Skalierung automatisch berechnen (mit 10% Puffer)
  float dataRange = maxWaterLevel - minWaterLevel;
  if (dataRange < 1.0) dataRange = 1.0;  // Mindestens 1 cm Bereich
  
  float buffer = dataRange * 0.1;  // 10% Puffer oben und unten
  float minY = minWaterLevel - buffer;
  float maxY = maxWaterLevel + buffer;
  
  // Auf 0.5 cm abrunden/aufrunden für schöne Werte
  minY = floor(minY * 2) / 2.0;
  maxY = ceil(maxY * 2) / 2.0;
  
  // Mindestbereich garantieren
  if (maxY - minY < 2.0) {
    float center = (minY + maxY) / 2.0;
    minY = center - 1.0;
    maxY = center + 1.0;
  }
  
  float rangeY = maxY - minY;
  
  // Achsen zeichnen
  tft.drawLine(chartX, chartY, chartX, chartY + chartHeight, COLOR_TEXT);  // Y-Achse
  tft.drawLine(chartX, chartY + chartHeight, chartX + chartWidth, chartY + chartHeight, COLOR_TEXT);  // X-Achse
  
  // Y-Achsen-Beschriftung (5 Schritte)
  tft.setTextSize(1);
  tft.setTextColor(COLOR_TEXT, COLOR_BACKGROUND);
  for (int i = 0; i <= 4; i++) {
    float yVal = minY + (rangeY * i / 4.0);
    int yPos = chartY + chartHeight - (yVal - minY) / rangeY * chartHeight;
    
    // Gitterlinie
    uint16_t gridColor = TFT_DARKGREY;
    tft.drawLine(chartX, yPos, chartX + chartWidth, yPos, gridColor);
    
    // Beschriftung
    tft.setCursor(chartX - 35, yPos - 4);
    tft.printf("%.1f", yVal);
    
    // Tick-Mark
    tft.drawLine(chartX - 3, yPos, chartX, yPos, COLOR_TEXT);
  }
  
  // Datenpunkte zeichnen (OPTIMIERT mit gesampleten Daten)
  int lastX = -1;
  int lastY = -1;
  
  // X-Achse von 0 bis JETZT (aktuelle Zeit), nicht bis zum letzten Datenpunkt!
  time_t nowTime = time(nullptr);
  unsigned long maxTimeFromStart = nowTime - currentCycleStartTime;
  
  // Mindestens 10 Minuten anzeigen
  if (maxTimeFromStart < 600) maxTimeFromStart = 600;
  
  for (int i = 0; i < sampledCount; i++) {
    DataPoint pt = sampledData[i];
    
    // Relative Zeit ab Zyklusstart berechnen
    unsigned long timeFromStart = pt.timestamp - currentCycleStartTime;
    
    // X-Position berechnen
    int x = chartX + (timeFromStart * chartWidth) / maxTimeFromStart;
    
    // Y-Position berechnen
    float normalizedY = (pt.waterLevel - minY) / rangeY;
    int y = chartY + chartHeight - (normalizedY * chartHeight);
    
    // Begrenzung
    if (y < chartY) y = chartY;
    if (y > chartY + chartHeight) y = chartY + chartHeight;
    
    // Linie zum vorherigen Punkt zeichnen
    if (lastX >= 0 && lastY >= 0) {
      uint16_t lineColor = pt.pumpActive ? COLOR_OK : TFT_BLUE;
      tft.drawLine(lastX, lastY, x, y, lineColor);
    }
    
    // Datenpunkt markieren
    uint16_t pointColor = pt.pumpActive ? COLOR_OK : TFT_BLUE;
    tft.fillCircle(x, y, 2, pointColor);
  
    lastX = x;
    lastY = y;
  }
  
  // X-Achsen-Beschriftung (DYNAMISCH basierend auf tatsächlicher Dauer)
  tft.setTextSize(1);
  tft.setTextColor(COLOR_TEXT, COLOR_BACKGROUND);
  
  // Intelligente Intervall-Berechnung basierend auf Gesamtdauer
  unsigned long labelInterval = 600;  // Standard: 10 Minuten
  
  if (maxTimeFromStart <= 3600) {
    // Bis 1 Stunde: alle 10 Minuten
    labelInterval = 600;
  } else if (maxTimeFromStart <= 7200) {
    // 1-2 Stunden: alle 15 Minuten
    labelInterval = 900;
  } else if (maxTimeFromStart <= 18000) {
    // 2-5 Stunden: alle 30 Minuten
    labelInterval = 1800;
  } else if (maxTimeFromStart <= 36000) {
    // 5-10 Stunden: alle 1 Stunde
    labelInterval = 3600;
  } else {
    // Über 10 Stunden: alle 2 Stunden
    labelInterval = 7200;
  }
  
  // Labels zeichnen - IMMER AB 0
  Serial.printf("X-Achsen-Beschriftung: labelInterval=%lu, maxTimeFromStart=%lu\n", labelInterval, maxTimeFromStart);
  for (unsigned long relativeTimeSeconds = 0; relativeTimeSeconds <= maxTimeFromStart; relativeTimeSeconds += labelInterval) {
    int x = chartX + (relativeTimeSeconds * chartWidth) / maxTimeFromStart;
    
    // Vertikale Gitterlinie
    uint16_t gridColor = TFT_DARKGREY;
    tft.drawLine(x, chartY, x, chartY + chartHeight, gridColor);
    
    // Tick-Mark an X-Achse
    tft.drawLine(x, chartY + chartHeight, x, chartY + chartHeight + 3, COLOR_TEXT);
    
    // Zeitstempel unter X-Achse - RELATIVE ZEIT
    char timeStr[16];
    
    if (relativeTimeSeconds == 0) {
      // Explizit "0" am Anfang
      sprintf(timeStr, "0");
    } else if (relativeTimeSeconds < 60) {
      // Sekunden
      sprintf(timeStr, "%lus", relativeTimeSeconds);
    } else if (relativeTimeSeconds < 3600) {
      // Nur Minuten
      unsigned long minutes = relativeTimeSeconds / 60;
      sprintf(timeStr, "%lumin", minutes);
    } else {
      // Stunden und Minuten
      unsigned long hours = relativeTimeSeconds / 3600;
      unsigned long minutes = (relativeTimeSeconds % 3600) / 60;
      if (minutes == 0) {
        sprintf(timeStr, "%luh", hours);
      } else {
        sprintf(timeStr, "%luh%02lu", hours, minutes);
      }
    }
    
    // Debug-Ausgabe
    Serial.printf("  Label: relativeTimeSeconds=%lu -> '%s' at x=%d\n", relativeTimeSeconds, timeStr, x);
    
    // Text zentrieren
    int textWidth = strlen(timeStr) * 6;  // Ungefähre Breite pro Zeichen
    int textX = x - (textWidth / 2);
    tft.setCursor(textX, chartY + chartHeight + 5);
    tft.print(timeStr);
  }
  
  // Aktueller Wert anzeigen
  tft.setTextSize(2);
  tft.setTextColor(COLOR_VALUE, COLOR_BACKGROUND);
  tft.setCursor(chartX, chartY + chartHeight + 30);
  tft.printf("Aktuell: %.1f cm", cisterne.waterLevel);
  
  // Pumpen-Status
  tft.setCursor(chartX + 200, chartY + chartHeight + 30);
  if (cisterne.pumpActive) {
    tft.setTextColor(COLOR_OK, COLOR_BACKGROUND);
    tft.print("Pumpe: AN");
  } else {
    tft.setTextColor(COLOR_TEXT, COLOR_BACKGROUND);
    tft.print("Pumpe: AUS");
  }
  
  // Anzahl Datenpunkte und Dauer
  tft.setTextSize(1);
  tft.setTextColor(COLOR_TEXT, COLOR_BACKGROUND);
  tft.setCursor(chartX, chartY + chartHeight + 55);
  tft.printf("Datenpunkte: %d", sampledCount);
  
  // Dauer des Pumpvorgangs anzeigen
  if (maxTimeFromStart > 0) {
    tft.setCursor(chartX + 150, chartY + chartHeight + 55);
    if (maxTimeFromStart < 60) {
      tft.printf("Dauer: %lus", maxTimeFromStart);
    } else if (maxTimeFromStart < 3600) {
      tft.printf("Dauer: %lumin", maxTimeFromStart / 60);
    } else {
      tft.printf("Dauer: %luh%02lumin", maxTimeFromStart / 3600, (maxTimeFromStart % 3600) / 60);
    }
  }
  
  // Legende
  tft.setCursor(chartX, chartY + chartHeight + 70);
  tft.setTextColor(TFT_BLUE, COLOR_BACKGROUND);
  tft.print("= Fuellen  ");
  tft.setTextColor(COLOR_OK, COLOR_BACKGROUND);
  tft.print("= Pumpen");
  
  // Speicher freigeben
  delete[] sampledData;
}

// JSON-Daten für Webseite
String getJsonData() {
  String json = "{";
  json += "\"waterLevel\":" + String(cisterne.waterLevel, 1) + ",";
  json += "\"adcValue\":" + String(cisterne.adcValue) + ",";
  json += "\"pumpActive\":" + String(cisterne.pumpActive ? "true" : "false") + ",";
  json += "\"pumpAlarm\":" + String(cisterne.pumpAlarm ? "true" : "false") + ",";
  json += "\"lastPumpDuration\":" + String(cisterne.lastPumpDuration) + ",";
  json += "\"lastUpdate\":\"" + lastUpdateTime + "\",";
  json += "\"dataReceived\":" + String(dataReceived ? "true" : "false") + ",";
  
  // Zeit zwischen Pumpvorgängen
  unsigned long timeBetweenPumps = 0;
  if (currentPumpStartTime > 0 && lastPumpStartTime > 0) {
    timeBetweenPumps = currentPumpStartTime - lastPumpStartTime;
  }
  json += "\"timeBetweenPumps\":" + String(timeBetweenPumps) + ",";
  json += "\"timeBetweenPumpsFormatted\":\"" + formatTimeDifference(timeBetweenPumps) + "\",";
  json += "\"lastPumpStartTime\":" + String(lastPumpStartTime) + ",";
  json += "\"currentPumpStartTime\":" + String(currentPumpStartTime) + ",";
  json += "\"currentCycleStartTime\":" + String(currentCycleStartTime);
  
  json += "}";
  return json;
}

// JSON-Daten für Chart (OPTIMIERT mit ArduinoJson und Sampling)
String getChartData() {
  // Schneller Exit wenn keine Daten
  if (currentCycleStartTime == 0 || historyCount == 0) {
    return "{\"currentPumpStartTime\":0,\"pointsCount\":0}";
  }
  
  // OPTIMIERT: Nur RAM-Daten verwenden (KEIN Archiv laden!)
  // Archiv ist für Webserver nicht nötig - RAM enthält die letzten ~1000 Punkte
  const int MAX_WEB_POINTS = 300;  // Maximal 300 Punkte für Browser
  
  // Zähle gültige Punkte (ab Zyklusstart) nur in RAM
  int validCount = 0;
  int ramStartIdx = (historyIndex - historyCount + MAX_HISTORY) % MAX_HISTORY;
  
  for (int i = 0; i < historyCount; i++) {
    int idx = (ramStartIdx + i) % MAX_HISTORY;
    if (history[idx].timestamp >= currentCycleStartTime) {
      validCount++;
    }
  }
  
  if (validCount == 0) {
    return "{\"currentPumpStartTime\":0,\"pointsCount\":0}";
  }
  
  // JSON mit ArduinoJson erstellen (effizienter als String-Concat!)
  DynamicJsonDocument doc(MAX_WEB_POINTS * 40 + 1024);
  
  JsonArray labels = doc.createNestedArray("labels");
  JsonArray waterLevels = doc.createNestedArray("waterLevels");
  JsonArray pumpStates = doc.createNestedArray("pumpStates");
  JsonArray relativeTime = doc.createNestedArray("relativeTime");
  JsonArray timestamps = doc.createNestedArray("timestamps");
  
  // Intelligentes Sampling
  int pointsAdded = 0;
  if (validCount <= MAX_WEB_POINTS) {
    // Alle Punkte senden
    for (int i = 0; i < historyCount; i++) {
      int idx = (ramStartIdx + i) % MAX_HISTORY;
      if (history[idx].timestamp >= currentCycleStartTime) {
        unsigned long relTime = history[idx].timestamp - currentCycleStartTime;
        labels.add(String(relTime));
        waterLevels.add(serialized(String(history[idx].waterLevel, 1)));
        pumpStates.add(history[idx].pumpActive);
        relativeTime.add(relTime);
        timestamps.add(history[idx].timestamp);
        pointsAdded++;
      }
    }
  } else {
    // Sampling: Jeden N-ten Punkt nehmen
    float step = (float)validCount / MAX_WEB_POINTS;
    int validIdx = 0;
    
    for (int i = 0; i < historyCount && pointsAdded < MAX_WEB_POINTS; i++) {
      int idx = (ramStartIdx + i) % MAX_HISTORY;
      if (history[idx].timestamp >= currentCycleStartTime) {
        // Nehme diesen Punkt wenn er auf dem Sample-Grid liegt
        if (validIdx == 0 || validIdx >= (int)(pointsAdded * step)) {
          unsigned long relTime = history[idx].timestamp - currentCycleStartTime;
          labels.add(String(relTime));
          waterLevels.add(serialized(String(history[idx].waterLevel, 1)));
          pumpStates.add(history[idx].pumpActive);
          relativeTime.add(relTime);
          timestamps.add(history[idx].timestamp);
          pointsAdded++;
        }
        validIdx++;
      }
    }
  }
  
  doc["pointsCount"] = pointsAdded;
  doc["currentPumpStartTime"] = currentCycleStartTime;
  doc["ramCount"] = historyCount;
  
  // Zu String serialisieren
  String output;
  serializeJson(doc, output);
  
  return output;
}

// JSON-Daten für Monats-Chart (alle Daten der letzten 30 Tage - inkl. Archiv)
String getChartDataMonth() {
  const int MAX_WEB_POINTS = 500;  // Maximal 500 Punkte für Monatsansicht
  const unsigned long MONTH_SECONDS = 30UL * 24UL * 3600UL;  // 30 Tage
  
  unsigned long now = time(nullptr);
  unsigned long monthAgo = now - MONTH_SECONDS;
  
  // Prüfe, ob wir Archiv-Daten benötigen
  bool needArchive = false;
  unsigned long oldestRamTime = 0;
  
  if (historyCount > 0) {
    int ramStartIdx = (historyIndex - historyCount + MAX_HISTORY) % MAX_HISTORY;
    oldestRamTime = history[ramStartIdx].timestamp;
    needArchive = (oldestRamTime > monthAgo && archiveStats.count > 0 && archiveStats.oldestTime < monthAgo);
  } else if (archiveStats.count > 0) {
    needArchive = true;
  } else {
    return "{\"pointsCount\":0}";
  }
  
  // JSON-Dokument für Ausgabe erstellen
  DynamicJsonDocument doc(MAX_WEB_POINTS * 40 + 1024);
  JsonArray waterLevels = doc.createNestedArray("waterLevels");
  JsonArray pumpStates = doc.createNestedArray("pumpStates");
  JsonArray timestamps = doc.createNestedArray("timestamps");
  
  int totalValidPoints = 0;
  
  // Schritt 1: Archiv-Daten zählen (falls benötigt)
  int archiveValidCount = 0;
  if (needArchive && LittleFS.exists(ARCHIVE_FILE)) {
    File file = LittleFS.open(ARCHIVE_FILE, "r");
    if (file) {
      DynamicJsonDocument archiveDoc(MAX_ARCHIVE * 64 + 2048);
      DeserializationError error = deserializeJson(archiveDoc, file);
      file.close();
      
      if (!error) {
        JsonArray archiveArray = archiveDoc["data"].as<JsonArray>();
        for (JsonObject point : archiveArray) {
          unsigned long ts = point["ts"] | 0;
          if (ts >= monthAgo && ts <= now) {
            archiveValidCount++;
          }
        }
      }
    }
  }
  
  // Schritt 2: RAM-Daten zählen
  int ramValidCount = 0;
  int ramStartIdx = (historyIndex - historyCount + MAX_HISTORY) % MAX_HISTORY;
  for (int i = 0; i < historyCount; i++) {
    int idx = (ramStartIdx + i) % MAX_HISTORY;
    if (history[idx].timestamp >= monthAgo && history[idx].timestamp <= now) {
      ramValidCount++;
    }
  }
  
  totalValidPoints = archiveValidCount + ramValidCount;
  
  if (totalValidPoints == 0) {
    return "{\"pointsCount\":0}";
  }
  
  // Schritt 3: Sampling-Strategie bestimmen
  float samplingStep = (totalValidPoints > MAX_WEB_POINTS) ? (float)totalValidPoints / MAX_WEB_POINTS : 1.0;
  int pointsAdded = 0;
  int validIdx = 0;
  
  // Schritt 4: Archiv-Daten sampeln und hinzufügen
  if (needArchive && LittleFS.exists(ARCHIVE_FILE)) {
    File file = LittleFS.open(ARCHIVE_FILE, "r");
    if (file) {
      DynamicJsonDocument archiveDoc(MAX_ARCHIVE * 64 + 2048);
      DeserializationError error = deserializeJson(archiveDoc, file);
      file.close();
      
      if (!error) {
        JsonArray archiveArray = archiveDoc["data"].as<JsonArray>();
        for (JsonObject point : archiveArray) {
          unsigned long ts = point["ts"] | 0;
          if (ts >= monthAgo && ts <= now) {
            // Sampling-Entscheidung
            if (samplingStep <= 1.0 || validIdx == 0 || validIdx >= (int)(pointsAdded * samplingStep)) {
              float wl = point["wl"] | 0.0f;
              bool pa = point["pa"] | false;
              
              waterLevels.add(serialized(String(wl, 1)));
              pumpStates.add(pa);
              timestamps.add(ts);
              pointsAdded++;
              
              if (pointsAdded >= MAX_WEB_POINTS) break;
            }
            validIdx++;
          }
        }
      }
    }
  }
  
  // Schritt 5: RAM-Daten sampeln und hinzufügen
  if (pointsAdded < MAX_WEB_POINTS) {
    for (int i = 0; i < historyCount && pointsAdded < MAX_WEB_POINTS; i++) {
      int idx = (ramStartIdx + i) % MAX_HISTORY;
      if (history[idx].timestamp >= monthAgo && history[idx].timestamp <= now) {
        // Sampling-Entscheidung
        if (samplingStep <= 1.0 || validIdx == 0 || validIdx >= (int)(pointsAdded * samplingStep)) {
          waterLevels.add(serialized(String(history[idx].waterLevel, 1)));
          pumpStates.add(history[idx].pumpActive);
          timestamps.add(history[idx].timestamp);
          pointsAdded++;
        }
        validIdx++;
      }
    }
  }
  
  doc["pointsCount"] = pointsAdded;
  doc["oldestTimestamp"] = pointsAdded > 0 ? timestamps[0].as<unsigned long>() : 0;
  doc["newestTimestamp"] = pointsAdded > 0 ? timestamps[pointsAdded - 1].as<unsigned long>() : 0;
  doc["usedArchive"] = needArchive;
  
  // Zu String serialisieren
  String output;
  serializeJson(doc, output);
  
  return output;
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== CisterneRegen & Dach Monitor ===");
  
  // LittleFS initialisieren
  Serial.println("Initialisiere LittleFS...");
  if (!LittleFS.begin(true)) {  // true = formatieren falls Mount fehlschlägt
    Serial.println("LittleFS Mount fehlgeschlagen!");
  } else {
    Serial.println("LittleFS erfolgreich gemountet");
    
    // Gespeicherte Daten laden
    loadAllData();
  }
  
  // TFT initialisieren
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);  // Backlight an
  
  tft.init();
  tft.setRotation(1);  // Landscape-Modus
  tft.fillScreen(COLOR_BACKGROUND);
  tft.setTextColor(COLOR_HEADER, COLOR_BACKGROUND);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("Starte...");
  
  // WiFi mit fester IP
  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("STA Failed to configure");
  }
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  Serial.print("Verbinde mit WiFi");
  tft.setCursor(10, 40);
  tft.setTextSize(1);
  tft.println("Verbinde WiFi...");
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi verbunden!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    
    tft.setCursor(10, 60);
    tft.setTextColor(COLOR_OK);
    tft.println("WiFi OK");
    tft.setCursor(10, 75);
    tft.setTextColor(COLOR_VALUE);
    tft.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi Verbindung fehlgeschlagen!");
    tft.setCursor(10, 60);
    tft.setTextColor(COLOR_ALARM);
    tft.println("WiFi Fehler!");
  }
  
  // NTP-Zeit initialisieren
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("NTP-Zeit wird synchronisiert...");
  tft.setCursor(10, 95);
  tft.setTextColor(COLOR_TEXT);
  tft.println("NTP sync...");
  
  // Warte auf NTP-Synchronisation
  struct tm timeinfo;
  int ntpAttempts = 0;
  while (!getLocalTime(&timeinfo) && ntpAttempts < 10) {
    delay(1000);
    ntpAttempts++;
  }
  
  if (getLocalTime(&timeinfo)) {
    Serial.println("NTP-Zeit synchronisiert");
    tft.setCursor(10, 110);
    tft.setTextColor(COLOR_OK);
    tft.println("NTP OK");
    
    // Nach Neustart: Verwende geladene Daten ODER setze currentCycleStartTime auf JETZT
    if (currentCycleStartTime == 0) {
      unsigned long now = time(nullptr);
      currentCycleStartTime = now;
      
      char timeStr[30];
      strftime(timeStr, sizeof(timeStr), "%d.%m.%Y %H:%M:%S", &timeinfo);
      Serial.printf("=== NEUSTART ohne gespeicherte Daten: Zyklusstart auf JETZT gesetzt: %s (timestamp=%lu) ===\n", timeStr, now);
    } else {
      char timeStr[30];
      time_t cycleTime = (time_t)currentCycleStartTime;
      struct tm cycleInfo;
      localtime_r(&cycleTime, &cycleInfo);
      strftime(timeStr, sizeof(timeStr), "%d.%m.%Y %H:%M:%S", &cycleInfo);
      Serial.printf("=== NEUSTART: Gespeicherte Daten wiederhergestellt - Zyklusstart: %s (timestamp=%lu) ===\n", timeStr, currentCycleStartTime);
      Serial.printf("=== Historie wiederhergestellt: %d Datenpunkte ===\n", historyCount);
    }
  }
  
  // Promiscuous Mode aktivieren für RSSI-Erfassung
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&promiscuous_rx_cb);
  Serial.println("Promiscuous Mode aktiviert (für RSSI)");
  
  // ESP-NOW initialisieren
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW Init Fehler");
    tft.setCursor(10, 130);
    tft.setTextColor(COLOR_ALARM);
    tft.println("ESP-NOW Fehler!");
    return;
  }
  
  Serial.println("ESP-NOW initialisiert");
  tft.setCursor(10, 130);
  tft.setTextColor(COLOR_OK);
  tft.println("ESP-NOW OK");
  
  // ESP-NOW Callback registrieren
  esp_now_register_recv_cb(OnDataRecv);
  
  // Webserver Routen
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Cisterne Monitor</title>
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
    <style>
        body {
            font-family: Arial, sans-serif;
            margin: 0;
            padding: 10px;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            min-height: 100vh;
            overflow: hidden;
        }
        .container {
            max-width: 1600px;
            margin: 0 auto;
            background: rgba(255, 255, 255, 0.1);
            backdrop-filter: blur(10px);
            border-radius: 15px;
            padding: 15px;
            box-shadow: 0 8px 32px rgba(0, 0, 0, 0.3);
            max-height: 98vh;
            overflow: hidden;
        }
        h1 {
            text-align: center;
            margin: 0 0 15px 0;
            font-size: 2em;
            text-shadow: 2px 2px 4px rgba(0, 0, 0, 0.3);
        }
        .main-layout {
            display: grid;
            grid-template-columns: 1fr 400px;
            gap: 20px;
            margin-top: 10px;
        }
        .chart-section {
            background: rgba(255, 255, 255, 0.95);
            border-radius: 15px;
            padding: 15px;
            max-height: 500px;
            display: flex;
            flex-direction: column;
        }
        .chart-section canvas {
            flex: 1;
            max-height: 450px;
        }
        .chart-controls {
            display: flex;
            justify-content: center;
            gap: 10px;
            margin-bottom: 15px;
        }
        .chart-button {
            padding: 10px 25px;
            border: none;
            border-radius: 10px;
            font-size: 1em;
            font-weight: bold;
            cursor: pointer;
            transition: all 0.3s;
            background: rgba(102, 126, 234, 0.3);
            color: #333;
        }
        .chart-button:hover {
            background: rgba(102, 126, 234, 0.5);
            transform: translateY(-2px);
        }
        .chart-button.active {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
        }
        .data-section {
            display: flex;
            flex-direction: column;
            gap: 10px;
            overflow-y: auto;
        }
        .data-card {
            background: rgba(255, 255, 255, 0.2);
            border-radius: 12px;
            padding: 12px;
            text-align: center;
        }
        .data-label {
            font-size: 0.85em;
            opacity: 0.8;
            margin-bottom: 5px;
        }
        .data-value {
            font-size: 1.8em;
            font-weight: bold;
            margin: 5px 0;
        }
        .data-value-small {
            font-size: 1.2em;
            font-weight: bold;
            margin: 5px 0;
        }
        .data-unit {
            font-size: 0.8em;
            opacity: 0.7;
        }
        .status {
            display: inline-block;
            padding: 8px 20px;
            border-radius: 20px;
            font-weight: bold;
            margin: 10px 0;
        }
        .status-ok {
            background: #10b981;
        }
        .status-alarm {
            background: #ef4444;
            animation: blink 1s infinite;
        }
        .status-off {
            background: #6b7280;
        }
        @keyframes blink {
            0%, 50%, 100% { opacity: 1; }
            25%, 75% { opacity: 0.5; }
        }
        .time-info {
            text-align: center;
            margin-top: 10px;
            padding: 10px;
            background: rgba(255, 255, 255, 0.1);
            border-radius: 10px;
            font-size: 0.85em;
        }
        @media (max-width: 1200px) {
            .main-layout {
                grid-template-columns: 1fr;
            }
        }
        /* Mobile Optimierungen - ab Tablet-Größe */
        @media (max-width: 768px) {
            body {
                overflow-y: auto;
                padding: 5px;
            }
            .container {
                max-height: none;
                overflow: visible;
                padding: 10px;
            }
            h1 {
                font-size: 1.5em;
                margin: 5px 0 10px 0;
            }
            .chart-section {
                max-height: 400px;
                padding: 10px;
            }
            .chart-section canvas {
                max-height: 350px;
            }
            .chart-controls {
                flex-wrap: wrap;
                gap: 8px;
            }
            .chart-button {
                padding: 8px 15px;
                font-size: 0.9em;
            }
            .data-card {
                padding: 10px;
            }
            .data-value {
                font-size: 1.5em;
            }
            .data-value-small {
                font-size: 1em;
            }
        }
        /* Smartphone-spezifische Optimierungen */
        @media (max-width: 480px) {
            body {
                padding: 5px;
            }
            .container {
                padding: 8px;
                border-radius: 10px;
            }
            h1 {
                font-size: 1.3em;
                margin: 5px 0 8px 0;
            }
            .main-layout {
                gap: 10px;
                margin-top: 5px;
            }
            .chart-section {
                max-height: 300px;
                padding: 8px;
            }
            .chart-section canvas {
                max-height: 250px;
            }
            .chart-controls {
                gap: 5px;
                margin-bottom: 10px;
            }
            .chart-button {
                padding: 6px 12px;
                font-size: 0.85em;
            }
            .data-section {
                gap: 8px;
            }
            .data-card {
                padding: 8px;
            }
            .data-label {
                font-size: 0.8em;
            }
            .data-value {
                font-size: 1.3em;
            }
            .data-value-small {
                font-size: 0.95em;
            }
            .data-unit {
                font-size: 0.75em;
            }
            .status {
                padding: 6px 15px;
                font-size: 0.9em;
            }
            .time-info {
                padding: 8px;
                font-size: 0.8em;
                margin-top: 8px;
            }
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>💧 Cisterne Monitor</h1>
        
        <div class="main-layout">
            <div class="chart-section">
                <div class="chart-controls">
                    <button class="chart-button active" id="btnCycle" onclick="switchToChartMode('cycle')">
                        📊 Aktueller Zyklus
                    </button>
                    <button class="chart-button" id="btnMonth" onclick="switchToChartMode('month')">
                        📅 Monatsübersicht
                    </button>
                </div>
                <canvas id="waterChart"></canvas>
            </div>
            
            <div class="data-section">
                <div class="data-card">
                    <div class="data-label">Wasserstand</div>
                    <div class="data-value" id="waterLevel">--</div>
                    <div class="data-unit">cm</div>
                </div>
                
                <div class="data-card">
                    <div class="data-label">Pumpen-Status</div>
                    <div id="pumpStatus" class="status status-off">AUS</div>
                </div>
                
                <div class="data-card">
                    <div class="data-label">Zeit zwischen Pumpvorgängen</div>
                    <div class="data-value-small" id="timeBetween">--</div>
                </div>
                
                <div class="data-card">
                    <div class="data-label">Füllzyklus Start</div>
                    <div class="data-value-small" id="cycleStart">--</div>
                </div>
                
                <div class="data-card">
                    <div class="data-label">ADC-Wert</div>
                    <div class="data-value" id="adcValue">--</div>
                </div>
                
                <div class="data-card">
                    <div class="data-label">Letzte Pumpdauer</div>
                    <div class="data-value-small" id="pumpDuration">--</div>
                </div>
                
                <div id="alarmCard" style="display:none;" class="data-card">
                    <div class="status status-alarm">⚠️ PUMPEN-ALARM ⚠️</div>
                </div>
            </div>
        </div>
        
        <div class="time-info">
            <div class="data-label">Letzte Aktualisierung</div>
            <div id="lastUpdate">Warte auf Daten...</div>
        </div>
    </div>

    <script>
        let chart = null;
        let lastPumpStartTime = 0;
        let chartMode = 'cycle';  // 'cycle' oder 'month'
        
        function switchToChartMode(mode) {
            chartMode = mode;
            
            // Button-Status aktualisieren
            document.getElementById('btnCycle').classList.remove('active');
            document.getElementById('btnMonth').classList.remove('active');
            
            if (mode === 'cycle') {
                document.getElementById('btnCycle').classList.add('active');
                updateChart();
            } else {
                document.getElementById('btnMonth').classList.add('active');
                updateMonthChart();
            }
        }
        
        function formatDuration(seconds) {
            if (!seconds || seconds === 0) return '--';
            const mins = Math.floor(seconds / 60);
            const secs = seconds % 60;
            return mins + ' min ' + secs + ' s';
        }
        
        function formatTimestamp(ts) {
            const date = new Date(ts * 1000);
            const hours = String(date.getHours()).padStart(2, '0');
            const minutes = String(date.getMinutes()).padStart(2, '0');
            const day = String(date.getDate()).padStart(2, '0');
            const month = String(date.getMonth() + 1).padStart(2, '0');
            
            return day + '.' + month + ' ' + hours + ':' + minutes;
        }
        
        function formatRelativeTime(seconds) {
            if (seconds < 60) {
                // Weniger als 1 Minute: Sekunden
                return seconds + 's';
            } else if (seconds < 3600) {
                // Weniger als 1 Stunde: Minuten:Sekunden
                const mins = Math.floor(seconds / 60);
                const secs = seconds % 60;
                if (secs === 0) {
                    return mins + 'min';
                }
                return mins + ':' + String(secs).padStart(2, '0');
            } else {
                // Mehr als 1 Stunde: Stunden:Minuten
                const hours = Math.floor(seconds / 3600);
                const mins = Math.floor((seconds % 3600) / 60);
                return hours + 'h' + String(mins).padStart(2, '0');
            }
        }
        
        function createChart(labels, waterLevels, pumpStates, timestamps, relativeTime, pumpStartTime) {
            const ctx = document.getElementById('waterChart').getContext('2d');
            
            // Formatierte Labels mit relativer Zeit (ab Pumpstart = 0)
            const formattedLabels = relativeTime.map(time => formatRelativeTime(time));
            
            // Titel mit Startzeit des Füllzyklus
            let chartTitle = 'Wasserstand-Verlauf (aktueller Füllzyklus)';
            if (pumpStartTime > 0) {
                const startTimeStr = formatTimestamp(pumpStartTime);
                chartTitle = 'Füllzyklus Start: ' + startTimeStr;
            }
            
            // Feste Y-Achsen-Skalierung für Zisterne
            let yMin = 10.0;  // Fester Minimalwert
            let yMax = 40.0;  // Fester Maximalwert
            
            // Hintergrundfarben basierend auf Pumpen-Status
            const backgroundColors = pumpStates.map(state => 
                state ? 'rgba(16, 185, 129, 0.2)' : 'rgba(99, 102, 241, 0.2)'
            );
            
            if (chart) {
                chart.destroy();
            }
            
            chart = new Chart(ctx, {
                type: 'line',
                data: {
                    labels: formattedLabels,
                    datasets: [{
                        label: 'Wasserstand (cm)',
                        data: waterLevels,
                        backgroundColor: 'rgba(99, 102, 241, 0.1)',
                        borderColor: 'rgba(99, 102, 241, 1)',
                        borderWidth: 3,
                        fill: true,
                        tension: 0.4,
                        pointBackgroundColor: pumpStates.map(state => 
                            state ? 'rgba(16, 185, 129, 1)' : 'rgba(99, 102, 241, 1)'
                        ),
                        pointBorderColor: '#fff',
                        pointBorderWidth: 1,
                        pointRadius: 2,
                        pointHoverRadius: 6
                    }]
                },
                options: {
                    responsive: true,
                    maintainAspectRatio: false,
                    plugins: {
                        legend: {
                            display: true,
                            position: 'top',
                            labels: {
                                color: '#333',
                                font: { size: 14 }
                            }
                        },
                        title: {
                            display: true,
                            text: chartTitle,
                            color: '#333',
                            font: { size: 16, weight: 'bold' }
                        },
                        tooltip: {
                            callbacks: {
                                label: function(context) {
                                    return 'Wasserstand: ' + context.parsed.y.toFixed(1) + ' cm';
                                },
                                afterLabel: function(context) {
                                    const pumpActive = pumpStates[context.dataIndex];
                                    const relTime = relativeTime[context.dataIndex];
                                    return pumpActive ? '🟢 Pumpe AKTIV (' + formatRelativeTime(relTime) + ')' 
                                                      : '⚪ Pumpe AUS (' + formatRelativeTime(relTime) + ')';
                                }
                            }
                        }
                    },
                    scales: {
                        y: {
                            beginAtZero: false,
                            min: yMin,
                            max: yMax,
                            ticks: {
                                color: '#333',
                                font: { size: 12 },
                                stepSize: 5,
                                callback: function(value) {
                                    return value.toFixed(0) + ' cm';
                                }
                            },
                            grid: { color: 'rgba(0, 0, 0, 0.1)' },
                            title: {
                                display: true,
                                text: 'Wasserstand (cm)',
                                color: '#333',
                                font: { size: 14, weight: 'bold' }
                            }
                        },
                        x: {
                            ticks: {
                                color: '#333',
                                font: { size: 11 },
                                maxRotation: 0,
                                minRotation: 0,
                                autoSkip: true,
                                maxTicksLimit: 10,
                                callback: function(value, index, ticks) {
                                    // Bessere Lesbarkeit durch intelligente Label-Auswahl
                                    const label = this.getLabelForValue(value);
                                    return label;
                                }
                            },
                            grid: { 
                                color: 'rgba(0, 0, 0, 0.1)',
                                drawOnChartArea: true
                            },
                            title: {
                                display: true,
                                text: 'Zeit seit Zyklusstart',
                                color: '#333',
                                font: { size: 14, weight: 'bold' }
                            }
                        }
                    }
                }
            });
        }
        
        function updateChart() {
            fetch('/chartdata')
                .then(response => response.json())
                .then(data => {
                    if (data.timestamps && data.timestamps.length > 0) {
                        // Prüfen ob Pumpe neu gestartet wurde
                        if (data.currentPumpStartTime > lastPumpStartTime) {
                            lastPumpStartTime = data.currentPumpStartTime;
                        }
                        
                        createChart(data.labels, data.waterLevels, data.pumpStates, 
                                  data.timestamps, data.relativeTime, data.currentPumpStartTime);
                    }
                })
                .catch(error => console.error('Error fetching chart data:', error));
        }
        
        function createMonthChart(waterLevels, pumpStates, timestamps) {
            const ctx = document.getElementById('waterChart').getContext('2d');
            
            // Formatierte Labels mit Datum/Uhrzeit
            const formattedLabels = timestamps.map(ts => formatTimestamp(ts));
            
            // Titel für Monatsansicht
            const oldestDate = new Date(timestamps[0] * 1000);
            const newestDate = new Date(timestamps[timestamps.length - 1] * 1000);
            const chartTitle = 'Monatsübersicht: ' + formatTimestamp(timestamps[0]) + ' - ' + 
                              formatTimestamp(timestamps[timestamps.length - 1]);
            
            // Feste Y-Achsen-Skalierung für Zisterne
            let yMin = 10.0;  // Fester Minimalwert
            let yMax = 40.0;  // Fester Maximalwert
            
            if (chart) {
                chart.destroy();
            }
            
            chart = new Chart(ctx, {
                type: 'line',
                data: {
                    labels: formattedLabels,
                    datasets: [{
                        label: 'Wasserstand (cm)',
                        data: waterLevels,
                        backgroundColor: 'rgba(99, 102, 241, 0.1)',
                        borderColor: 'rgba(99, 102, 241, 1)',
                        borderWidth: 2,
                        fill: true,
                        tension: 0.3,
                        pointBackgroundColor: pumpStates.map(state => 
                            state ? 'rgba(16, 185, 129, 1)' : 'rgba(99, 102, 241, 1)'
                        ),
                        pointBorderColor: '#fff',
                        pointBorderWidth: 1,
                        pointRadius: 1,
                        pointHoverRadius: 5
                    }]
                },
                options: {
                    responsive: true,
                    maintainAspectRatio: false,
                    plugins: {
                        legend: {
                            display: true,
                            position: 'top',
                            labels: {
                                color: '#333',
                                font: { size: 14 }
                            }
                        },
                        title: {
                            display: true,
                            text: chartTitle,
                            color: '#333',
                            font: { size: 16, weight: 'bold' }
                        },
                        tooltip: {
                            callbacks: {
                                label: function(context) {
                                    return 'Wasserstand: ' + context.parsed.y.toFixed(1) + ' cm';
                                },
                                afterLabel: function(context) {
                                    const pumpActive = pumpStates[context.dataIndex];
                                    const ts = timestamps[context.dataIndex];
                                    return pumpActive ? '🟢 Pumpe AKTIV' : '⚪ Pumpe AUS';
                                }
                            }
                        }
                    },
                    scales: {
                        y: {
                            beginAtZero: false,
                            min: yMin,
                            max: yMax,
                            ticks: {
                                color: '#333',
                                font: { size: 12 },
                                stepSize: 5,
                                callback: function(value) {
                                    return value.toFixed(0) + ' cm';
                                }
                            },
                            grid: { color: 'rgba(0, 0, 0, 0.1)' },
                            title: {
                                display: true,
                                text: 'Wasserstand (cm)',
                                color: '#333',
                                font: { size: 14, weight: 'bold' }
                            }
                        },
                        x: {
                            ticks: {
                                color: '#333',
                                font: { size: 10 },
                                maxRotation: 45,
                                minRotation: 45,
                                autoSkip: true,
                                maxTicksLimit: 15
                            },
                            grid: { 
                                color: 'rgba(0, 0, 0, 0.05)',
                                drawOnChartArea: true
                            },
                            title: {
                                display: true,
                                text: 'Datum / Uhrzeit',
                                color: '#333',
                                font: { size: 14, weight: 'bold' }
                            }
                        }
                    }
                }
            });
        }
        
        function updateMonthChart() {
            fetch('/chartdata_month')
                .then(response => response.json())
                .then(data => {
                    if (data.timestamps && data.timestamps.length > 0) {
                        createMonthChart(data.waterLevels, data.pumpStates, data.timestamps);
                    }
                })
                .catch(error => console.error('Error fetching month chart data:', error));
        }
        
        function updateData() {
            fetch('/data')
                .then(response => response.json())
                .then(data => {
                    document.getElementById('waterLevel').textContent = 
                        data.dataReceived ? data.waterLevel.toFixed(1) : '--';
                    document.getElementById('adcValue').textContent = 
                        data.dataReceived ? data.adcValue : '--';
                    document.getElementById('pumpDuration').textContent = 
                        formatDuration(data.lastPumpDuration);
                    document.getElementById('lastUpdate').textContent = data.lastUpdate;
                    
                    // Zeit zwischen Pumpvorgängen
                    document.getElementById('timeBetween').textContent = 
                        data.timeBetweenPumpsFormatted || '--';
                    
                    // Füllzyklus Startzeit
                    if (data.currentCycleStartTime && data.currentCycleStartTime > 0) {
                        document.getElementById('cycleStart').textContent = 
                            formatTimestamp(data.currentCycleStartTime);
                    } else {
                        document.getElementById('cycleStart').textContent = '--';
                    }
                    
                    // Pumpen-Status
                    const pumpStatus = document.getElementById('pumpStatus');
                    if (data.pumpActive) {
                        pumpStatus.textContent = 'AKTIV';
                        pumpStatus.className = 'status status-ok';
                    } else {
                        pumpStatus.textContent = 'AUS';
                        pumpStatus.className = 'status status-off';
                    }
                    
                    // Alarm
                    const alarmCard = document.getElementById('alarmCard');
                    if (data.pumpAlarm) {
                        alarmCard.style.display = 'block';
                    } else {
                        alarmCard.style.display = 'none';
                    }
                })
                .catch(error => console.error('Error:', error));
        }
        
        // Initial update
        updateData();
        updateChart();
        
        // Update data every 2 seconds
        setInterval(updateData, 2000);
        
        // Update chart every 10 seconds (mit Modi-Check)
        setInterval(function() {
            if (chartMode === 'cycle') {
                updateChart();
            } else if (chartMode === 'month') {
                updateMonthChart();
            }
        }, 10000);
    </script>
</body>
</html>
)rawliteral");
  });
  
  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "application/json", getJsonData());
  });
  
  server.on("/chartdata", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "application/json", getChartData());
  });
  
  server.on("/chartdata_month", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "application/json", getChartDataMonth());
  });
  
  server.begin();
  Serial.println("Webserver gestartet");
  
  tft.setCursor(10, 150);
  tft.setTextColor(COLOR_OK);
  tft.println("Webserver OK");
  
  delay(2000);
  
  // Erstes Display-Update
  updateDisplay();
  
  Serial.println("\n=== System bereit ===");
  Serial.print("Webseite: http://");
  Serial.println(WiFi.localIP());
}

unsigned long lastDisplayUpdate = 0;
const unsigned long DISPLAY_UPDATE_INTERVAL_STATUS = 2000;   // Status: Alle 2 Sekunden
const unsigned long DISPLAY_UPDATE_INTERVAL_CHART = 30000;   // Chart: Alle 30 Sekunden (OPTIMIERT!)

void loop() {
  // Display-Modus automatisch wechseln (alle 10 Sekunden)
  if (millis() - lastModeSwitch >= MODE_SWITCH_INTERVAL) {
    // Modus wechseln
    if (currentDisplayMode == DISPLAY_MODE_STATUS) {
      currentDisplayMode = DISPLAY_MODE_CHART;
      displayInitialized = false;  // Neu initialisieren beim Wechsel
    } else {
      currentDisplayMode = DISPLAY_MODE_STATUS;
      displayInitialized = false;  // Neu initialisieren beim Wechsel
    }
    lastModeSwitch = millis();
    lastDisplayUpdate = 0;  // Sofort aktualisieren
  }
  
  // Display periodisch aktualisieren (unterschiedliche Intervalle!)
  unsigned long updateInterval = (currentDisplayMode == DISPLAY_MODE_STATUS) 
                                  ? DISPLAY_UPDATE_INTERVAL_STATUS 
                                  : DISPLAY_UPDATE_INTERVAL_CHART;
  
  if (millis() - lastDisplayUpdate >= updateInterval) {
    if (currentDisplayMode == DISPLAY_MODE_STATUS) {
      updateDisplay();
    } else {
      drawChart();
    }
    lastDisplayUpdate = millis();
  }
  
  // Daten periodisch auf LittleFS speichern (alle 5 Minuten)
  if (millis() - lastSaveTime >= SAVE_INTERVAL) {
    Serial.println("Periodisches Speichern...");
    saveAllData();
    lastSaveTime = millis();
  }
  
  // Kurze Pause
  delay(10);
}
