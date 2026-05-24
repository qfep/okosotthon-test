#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h> 
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <ArduinoJson.h>

// OneWireNg Dallas-kompatibilis réteg
#include <OneWire.h> 
#include <DallasTemperature.h>

// --- Beállítások ---
const char* ssid     = "TP-Link_DCAD";
const char* password = "34012291";

const char* apiKey   = "d0a9b57e99781834f4d9e61beb9b721a";
const char* city     = "Domsod,HU"; 

const int RELAY_PIN = 4; 
const int ONE_WIRE_BUS = 5; 

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// --- Globális változók ---
String weatherTemp = "--";
String weatherHumidity = "--";
String weatherWind = "--";
String weatherRain = "0";
String weatherDesc = "Betöltés...";
String weatherIcon = "01d"; 

struct Forecast {
  String dayName = "---";
  String temp = "--";
  String desc = "...";
  String icon = "01d";
};
Forecast forecasts[3];

float t1 = 0.0, t2 = 0.0, t3 = 0.0, t4 = 0.0, t5 = 0.0;

// Üzemmód változók: false = TÉLI (Fűtés), true = NYÁRI (Hűtés)
bool isSummerMode = false; 
int lastCheckedDay = -1; // Figyeli, hogy új napra ébredtünk-e

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000); 

WebServer server(80);

// --- Automata Nyári/Téli üzemmód kalkulátor ---
// Május 5 (hónap: 5, nap: 5) és Szeptember 15 (hónap: 9, nap: 15) között igazat ad vissza
bool calculateAutoSeason(int month, int day) {
  int currentMatch = (month * 100) + day; // Pl: Május 5 -> 505, Szeptember 15 -> 915
  if (currentMatch >= 505 && currentMatch < 915) {
    return true;  // Nyári (Hűtés) időszak
  }
  return false;   // Téli (Fűtés) időszak
}

// --- Időzóna (Magyarországra fixálva) ---
bool checkIsDST(unsigned long utcEpoch) {
  time_t rawtime = utcEpoch;
  struct tm * ti = gmtime(&rawtime);
  int month = ti->tm_mon + 1;
  int day = ti->tm_mday;
  int hour = ti->tm_hour;
  int wday = ti->tm_wday; 
  if (month < 3 || month > 10) return false; 
  if (month > 3 && month < 10) return true; 
  int previousSunday = day - wday;
  if (month == 3) {
    if (previousSunday >= 25 && (wday != 0 || hour >= 1)) return true;
    return false;
  } else {
    if (previousSunday >= 25 && (wday != 0 || hour >= 1)) return false;
    return true;
  }
}

unsigned long getLocalEpoch(unsigned long utcEpoch) {
  return checkIsDST(utcEpoch) ? utcEpoch + 7200 : utcEpoch + 3600;
}

String getFormattedDate(unsigned long localEpoch) {
  time_t rawtime = localEpoch;
  struct tm * ti = gmtime(&rawtime);
  char buffer[20];
  sprintf(buffer, "%04d.%02d.%02d.", ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday);
  return String(buffer);
}

String getDayOfWeekName(int wday) {
  const char* days[] = {"Vasárnap", "Hétfő", "Kedd", "Szerda", "Csütörtök", "Péntek", "Szombat"};
  if (wday >= 0 && wday < 7) return String(days[wday]);
  return "---";
}

// --- Időjárás lekérés ---
void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure(); 

  String url = "https://api.openweathermap.org/data/2.5/weather?q=" + String(city) + "&units=metric&lang=hu&appid=" + String(apiKey);
  if (http.begin(client, url)) {
    int httpCode = http.GET();
    if (httpCode == 200) {
      String payload = http.getString();
      JsonDocument doc;
      deserializeJson(doc, payload);
      weatherTemp = String((float)doc["main"]["temp"], 1);
      weatherHumidity = String((int)doc["main"]["humidity"]);
      weatherWind = String((float)doc["wind"]["speed"] * 3.6, 1); 
      weatherRain = String(doc["rain"]["1h"].as<float>(), 1); 
      
      const char* desc = doc["weather"][0]["description"];
      const char* icon = doc["weather"][0]["icon"];
      weatherDesc = String(desc);
      weatherDesc.toUpperCase();
      weatherIcon = String(icon);
    }
    http.end();
  }

  String forecastUrl = "https://api.openweathermap.org/data/2.5/forecast?q=" + String(city) + "&units=metric&lang=hu&appid=" + String(apiKey);
  if (http.begin(client, forecastUrl)) {
    int httpCode = http.GET();
    if (httpCode == 200) {
      String payload = http.getString();
      DynamicJsonDocument doc(24576); 
      deserializeJson(doc, payload);
      
      int fCount = 0;
      JsonArray list = doc["list"];
      
      for (JsonObject item : list) {
        String dt_txt = item["dt_txt"].as<String>();
        if (dt_txt.endsWith("15:00:00") && fCount < 3) {
          unsigned long timestamp = item["dt"].as<unsigned long>();
          time_t rawTime = getLocalEpoch(timestamp);
          struct tm * ti = gmtime(&rawTime);
          
          forecasts[fCount].dayName = getDayOfWeekName(ti->tm_wday);
          forecasts[fCount].temp = String((float)item["main"]["temp"], 1);
          const char* fDesc = item["weather"][0]["description"];
          forecasts[fCount].desc = String(fDesc);
          forecasts[fCount].icon = item["weather"][0]["icon"].as<String>();
          fCount++;
        }
      }
    }
    http.end();
  }
}

void readSensors() {
  sensors.requestTemperatures();
  t1 = sensors.getTempCByIndex(0);
  t2 = sensors.getTempCByIndex(1);
  t3 = sensors.getTempCByIndex(2);
  t4 = sensors.getTempCByIndex(3);
  t5 = sensors.getTempCByIndex(4);
}

// --- Weboldal HTML ---
void handleRoot() {
  readSensors(); 
  unsigned long utcEpoch = timeClient.getEpochTime();
  unsigned long localEpoch = getLocalEpoch(utcEpoch);
  
  time_t rawtime = localEpoch;
  struct tm * ti = gmtime(&rawtime);
  
  String html = "<!DOCTYPE html><html lang='hu'>";
  html += "<head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>Időkép Állomás & Vezérlő</title>";
  html += "<style>";
  html += "body { font-family: 'Segoe UI', Arial, sans-serif; background: linear-gradient(135deg, #0f4c81 0%, #1d70b8 100%); color: #333; margin: 0; padding: 15px; display: flex; justify-content: center; align-items: center; min-height: 100vh; }";
  html += ".container { width: 100%; max-width: 500px; }";
  html += ".card { background: rgba(255, 255, 255, 0.95); padding: 20px; border-radius: 20px; box-shadow: 0 10px 30px rgba(0,0,0,0.15); margin-bottom: 15px; box-sizing: border-box; }";
  html += "h1 { margin: 0; color: #1d70b8; font-size: 24px; font-weight: 800; text-transform: uppercase; letter-spacing: 1px; }";
  html += ".date { font-size: 16px; color: #555; font-weight: bold; margin-top: 5px; }";
  html += ".time { font-size: 46px; font-weight: bold; color: #111; margin: 5px 0; font-variant-numeric: tabular-nums; }";
  html += ".section-title { font-size: 13px; font-weight: bold; text-align: left; color: #1d70b8; text-transform: uppercase; margin-bottom: 12px; border-bottom: 2px solid #1d70b8; padding-bottom: 3px; letter-spacing: 0.5px; }";
  html += ".idokep-main { display: flex; align-items: center; justify-content: space-around; padding: 10px 0; }";
  html += ".idokep-temp { font-size: 48px; font-weight: 800; color: #1d70b8; }";
  html += ".idokep-icon { width: 90px; height: 90px; filter: drop-shadow(2px 4px 6px rgba(0,0,0,0.1)); }";
  html += ".grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; text-align: left; }";
  html += ".data-item { background: #f0f4f8; padding: 10px 12px; border-radius: 10px; border-left: 4px solid #1d70b8; }";
  html += ".ds-item { border-left-color: #2e7d32; background: #f1f8e9; }";
  html += ".label { font-size: 11px; color: #666; text-transform: uppercase; font-weight: 600; }";
  html += ".value { font-size: 16px; font-weight: bold; color: #222; margin-top: 2px; }";
  html += ".forecast-row { display: flex; align-items: center; justify-content: space-between; padding: 8px 10px; margin-bottom: 8px; background: #fcfcfc; border-radius: 10px; box-shadow: inset 0 0 5px rgba(0,0,0,0.02); }";
  html += ".fc-day { width: 30%; font-weight: bold; text-align: left; }";
  html += ".fc-info { width: 45%; font-size: 13px; color: #555; text-align: left; text-transform: capitalize; }";
  html += ".fc-temp-box { width: 25%; font-weight: bold; color: #ff6d00; text-align: right; font-size: 16px; }";
  html += ".fc-icon { width: 40px; height: 40px; }";
  html += ".status-box { padding: 12px; border-radius: 10px; font-weight: bold; font-size: 16px; margin: 10px 0; text-align: center; ";
  html += isSummerMode ? "background: #fff3e0; color: #e65100; border: 1px solid #ffe0b2;" : "background: #e3f2fd; color: #0d47a1; border: 1px solid #bbdefb;";
  html += "}";
  html += ".btn { display: inline-block; width: 100%; padding: 12px; margin-top: 8px; border: none; border-radius: 10px; font-size: 15px; font-weight: bold; cursor: pointer; text-decoration: none; text-align: center; box-sizing: border-box; transition: 0.2s; box-shadow: 0 4px 10px rgba(0,0,0,0.1); }";
  html += isSummerMode ? ".btn-toggle { background: #0d47a1; color: white; }" : ".btn-toggle { background: #e65100; color: white; }";
  html += ".info-msg { font-size: 11px; color: #777; text-align: center; margin-top: 5px; }";
  html += "</style></head><body>";
  
  html += "<div class='container'>";
  
  // 1. Óra panel
  html += "<div class='card'>";
  html += "<h1>Időkép Panel</h1>";
  html += "<div class='date'>" + getFormattedDate(localEpoch) + "</div>";
  html += "<div class='time' id='clock'>--:--:--</div>";
  html += "</div>";
  
  // 2. OpenWeather adatok
  html += "<div class='card'>";
  html += "<div class='section-title'>Aktuális Időjárás (" + String(city) + ")</div>";
  html += "<div class='idokep-main'>";
  html += "  <img class='idokep-icon' src='https://openweathermap.org/img/wn/" + weatherIcon + "@2x.png' alt='icon'>";
  html += "  <div class='idokep-temp'>" + weatherTemp + "°C</div>";
  html += "</div>";
  html += "<div style='text-align:center; font-weight:bold; color:#1d70b8; margin-bottom:15px; font-size:15px;'>" + weatherDesc + "</div>";
  
  html += "<div class='grid'>";
  html += "  <div class='data-item'><div class='label'>💧 Páratartalom</div><div class='value'>" + weatherHumidity + " %</div></div>";
  html += "  <div class='data-item'><div class='label'>💨 Szélsebesség</div><div class='value'>" + weatherWind + " km/h</div></div>";
  html += "  <div class='data-item'><div class='label'>🌧️ Eső (1 órás)</div><div class='value'>" + weatherRain + " mm</div></div>";
  html += "</div>";
  html += "</div>";

  // 3. Előrejelzés
  html += "<div class='card'>";
  html += "<div class='section-title'>3 Napos Előrejelzés (Délután)</div>";
  for (int i = 0; i < 3; i++) {
    html += "<div class='forecast-row'>";
    html += "  <div class='fc-day'>" + forecasts[i].dayName + "</div>";
    html += "  <img class='fc-icon' src='https://openweathermap.org/img/wn/" + forecasts[i].icon + ".png'>";
    html += "  <div class='fc-info'>" + forecasts[i].desc + "</div>";
    html += "  <div class='fc-temp-box'>" + forecasts[i].temp + "°C</div>";
    html += "</div>";
  }
  html += "</div>";

  // 4. Helyi DS18B20 mérők
  html += "<div class='card'>";
  html += "<div class='section-title'>Helyi Hőmérők (DS18B20)</div>";
  html += "<div class='grid'>";
  html += "  <div class='data-item ds-item'><div class='label'>T1 Hőmérő</div><div class='value'>" + (t1 == DEVICE_DISCONNECTED_C ? "Hiba" : String(t1, 1) + " °C") + "</div></div>";
  html += "  <div class='data-item ds-item'><div class='label'>T2 Hőmérő</div><div class='value'>" + (t2 == DEVICE_DISCONNECTED_C ? "Hiba" : String(t2, 1) + " °C") + "</div></div>";
  html += "  <div class='data-item ds-item'><div class='label'>T3 Hőmérő</div><div class='value'>" + (t3 == DEVICE_DISCONNECTED_C ? "Hiba" : String(t3, 1) + " °C") + "</div></div>";
  html += "  <div class='data-item ds-item'><div class='label'>T4 Hőmérő</div><div class='value'>" + (t4 == DEVICE_DISCONNECTED_C ? "Hiba" : String(t4, 1) + " °C") + "</div></div>";
  html += "  <div class='data-item ds-item'><div class='label'>T5 Hőmérő</div><div class='value'>" + (t5 == DEVICE_DISCONNECTED_C ? "Hiba" : String(t5, 1) + " °C") + "</div></div>";
  html += "</div>";
  html += "</div>";

  // 5. Rendszer és Relé Vezérlés
  html += "<div class='card'>";
  html += "<div class='section-title'>Rendszer Vezérlés</div>";
  html += "<div class='status-box'>Aktuális üzemmód: " + String(isSummerMode ? "☀️ NYÁRI (Hűtés)" : "❄️ TÉLI (Fűtés)") + "</div>";
  html += "<a href='/toggle' class='btn btn-toggle'>" + String(isSummerMode ? "Átváltás FŰTÉSRE" : "Átváltás HŰTÉSRE") + "</a>";
  html += "<div class='info-msg'>Automatikus naptár: Máj. 5 - Szept. 15 hűtés, különben fűtés. A gombbal felülbírálható.</div>";
  html += "</div>";
  
  html += "</div>"; 

  html += "<script>";
  html += "let h = " + String(ti->tm_hour) + ";";
  html += "let m = " + String(ti->tm_min) + ";";
  html += "let s = " + String(ti->tm_sec) + ";";
  html += "function updateClock() {";
  html += "  s++;";
  html += "  if(s >= 60) { s = 0; m++; if(m >= 60) { m = 0; h++; if(h >= 24) { h = 0; } } }";
  html += "  document.getElementById('clock').innerHTML = String(h).padStart(2,'0') + ':' + String(m).padStart(2,'0') + ':' + String(s).padStart(2,'0');";
  html += "}";
  html += "setInterval(updateClock, 1000); updateClock();";
  html += "setInterval(function(){ location.reload(); }, 60000);"; 
  html += "</script>";
  
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

// Manuális gombnyomás kezelése
void handleToggle() {
  isSummerMode = !isSummerMode;
  digitalWrite(RELAY_PIN, isSummerMode ? HIGH : LOW); 
  server.sendHeader("Location", "/");
  server.send(303);
}

void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW); 

  sensors.begin();

  IPAddress dns(8, 8, 8, 8); 
  WiFi.begin(ssid, password);
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, dns);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  timeClient.begin();
  timeClient.update();
  fetchWeather();

  server.on("/", handleRoot);
  server.on("/toggle", handleToggle);
  
  server.begin();
}

void loop() {
  server.handleClient();
  timeClient.update(); 

  // --- Automatikus naptári ellenőrzés naponta egyszer ---
  unsigned long localEpoch = getLocalEpoch(timeClient.getEpochTime());
  time_t rawtime = localEpoch;
  struct tm * ti = gmtime(&rawtime);
  int currentDay = ti->tm_mday;
  int currentMonth = ti->tm_mon + 1;

  // Ha új nap lett (vagy indításkor), az automatika beállítja az alapértelmezést
  if (currentDay != lastCheckedDay && ti->tm_hour == 0) { 
    lastCheckedDay = currentDay;
    isSummerMode = calculateAutoSeason(currentMonth, currentDay);
    digitalWrite(RELAY_PIN, isSummerMode ? HIGH : LOW);
  }

  // Időjárás frissítése 15 percenként
  static unsigned long lastWeatherUpdate = 0;
  if (millis() - lastWeatherUpdate >= 900000) { 
    lastWeatherUpdate = millis();
    fetchWeather();
  }
}