# CisterneRegen & Dach Monitor

## Übersicht
Dieses Projekt empfängt **nur die Cisternendaten** via ESP-NOW und zeigt sie auf einem TFT-Display sowie auf einer Webseite an. Die Daten werden mit NTP-Zeit versehen.

## Features
- ✅ ESP-NOW Empfang von `WaterLevelData` Struktur
- ✅ NTP-Zeit Synchronisation
- ✅ TFT-Display Anzeige (ILI9341)
- ✅ Responsive Webseite mit Auto-Refresh
- ✅ Feste IP-Konfiguration

## Hardware
- ESP32 Development Board
- ILI9341 TFT Display (240x320)
- WiFi-Verbindung für NTP und Webserver

## Angezeigte Daten
- **Wasserstand** (cm)
- **ADC-Wert** (raw)
- **Pumpen-Status** (aktiv/aus)
- **Pumpen-Alarm** (falls aktiv)
- **Letzte Pumpdauer** (Sekunden)
- **Letzte Aktualisierung** (Datum/Zeit von NTP)

## Pin-Konfiguration (TFT)
```
TFT_MISO = 19
TFT_MOSI = 23
TFT_SCLK = 18
TFT_CS   = 15
TFT_DC   = 2
TFT_RST  = 4
TFT_BL   = 27 (Backlight)
```

## Konfiguration
In `main.cpp` anpassen:
```cpp
// WiFi
const char* ssid = "Lenovo";
const char* password = "lenovotablet";

// Feste IP
IPAddress local_IP(192, 168, 100, 150);
IPAddress gateway(192, 168, 100, 1);
```

## Installation
1. Projekt in PlatformIO öffnen
2. WiFi-Daten in `main.cpp` anpassen
3. Build & Upload
4. Webseite unter `http://192.168.100.150` aufrufen

## ESP-NOW Sender
Der Sender muss die identische `WaterLevelData` Struktur senden:
```cpp
typedef struct {
  float waterLevel;
  int adcValue;
  bool pumpActive;
  bool pumpAlarm;
  unsigned long pumpReferenceTime;
  unsigned long lastPumpDuration;
} WaterLevelData;
```

## Webseite
- Auto-Refresh alle 2 Sekunden
- Responsive Design
- Visueller Alarm bei Pumpen-Alarm
- Anzeige aller Cisternendaten mit NTP-Zeit

## Autor
doctorPit2

## Lizenz
MIT
