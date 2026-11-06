// -------------------------------  СИСТЕМА АВТОПОЛИВА  -------------------------------
//                             ДЛЯ АВТОНОМНОГО ИСПОЛЬЗОВАНИЯ
//                              УПРАВЛЯЕМАЯ VK СООБЩЕСТВОМ
//                             НА БАЗЕ КАСТОМНОЙ БИБЛИОТЕКИ
//                                    "VKassistent"
//                                   by Arctic_WAWES
//
//  Прошивка для серии моделей ArtWat (home edition)
//  Firmwork version: 1.9 full
//  Microcontroller: Wemos D1 R32 (ESP32 "UNO")
//
//   Особенности:
//  - Подключение устройства происходит с помощью локальной HTML страницы, открываемой после
//  подключения к создаваемой микроконтроллером точке доступа, на ней вводятся параметры
//  сети WiFi и пользовательского VK сообщества.
//  - За счёт использования кастомной библиотеки в код не выносятся HTTP и Json 
//  настройки, которые сильно ухудшали визуал и чтение кода.
//  - В случае неполадок в работе сервисов VK или внутренних поломок считывать показания
//  и управлять системой можно с помощью Serial монитора, войдя в режим администратора 
//  отправляя команды через кабель на микроконтроллер напрямую с любого устройства.
//  - Автосохранение всех внесённых параметров, начиная от системного времени, заканчивая
//  временем включения насоса, лампы и т.д.
//  - ПОЛНЫЙ цикл автономной работы, при непредвиденной потери сети система входит в "FullAuto" 
//  режим где насос и лампа включаются по системному времени внезависимости от наличия интернета,
//  но функия работает только при внесённых параметрах полива и света, если они отсуствуют -
//  система не будет включаться.
//  - Световая индикация в качестве всторенного на плату светодиода.
//

#include <VKassistent.h>
#include <Wire.h>
#include <RTClib.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <Update.h>
#include <esp_system.h>

const char* AP_SSID = "ArtWAT_system";
const char* AP_PASSWORD = "3468#2550";
const String SERIAL_PASSWORD = "3468#2550";

// I2C пины для DS3231
const int SDA_PIN = 21;
const int SCL_PIN = 22;

RTC_DS3231 rtc;
bool rtcReady = false;
unsigned long lastRtcRecoveryAttempt = 0;
const unsigned long RTC_RECOVERY_INTERVAL = 30000;

const int PUMP_PIN = 4;
const int LIGHT_PIN = 5;

// ===== СВЕТОДИОД ИНДИКАЦИИ =====
const int STATUS_LED_PIN = 2;

// ===== ТИП РЕЛЕ =====
#define RELAY_ACTIVE_LOW true

// ===== ПРЕДОХРАНИТЕЛЬ НАСОСА =====
const unsigned long PUMP_ABSOLUTE_MAX_MS = 30UL * 60UL * 1000UL;

WebServer server(80);
DNSServer dnsServer;
const byte DNS_PORT = 53;
IPAddress apIP(192, 168, 4, 1);
Preferences preferences;

// ===== СОСТОЯНИЕ СИСТЕМЫ =====
bool apMode = false;
bool staConnected = false;
bool configMode = false;
unsigned long lastWifiAttempt = 0;
unsigned long wifiLostTime = 0;

// ===== АСИНХРОННОЕ ПОДКЛЮЧЕНИЕ К WI-FI =====
bool wifiConnecting = false;
unsigned long wifiConnectStart = 0;
const unsigned long WIFI_CONNECT_TIMEOUT = 20000;

VKassistent* bot = nullptr;

// ===== СЛОТЫ ВРЕМЕНИ ПОЛИВА =====
#define WATER_SLOTS 3
String waterSlots[WATER_SLOTS] = {"", "", ""};
int activeWaterSlot = -1;
unsigned long lastWaterTriggerMs = 0;

// ===== ПАРАМЕТРЫ СИСТЕМЫ =====
String current_keyboard = "MainMenu";
String lightTime = "18:00";
int light_duration_value = 0;
int water_duration_value = 0;
bool waitingForWaterTime = false;
long waitingUserId = 0;
bool waitingForLightTime = false;
long waitingLightUserId = 0;
bool lightState = false;
bool pumpState = false;
unsigned long lightStartTime = 0;
unsigned long pumpStartTime = 0;

// ===== РУЧНОЙ ПОЛИВ =====
bool waitingForManualWaterTime = false;
long waitingManualUserId = 0;
bool manualWaterActive = false;
unsigned long manual_water_duration_value = 0;

// ===== ФЛАГИ SERIAL =====
bool waitingForDeleteConfirm = false;
bool serialAuthorized = false;
bool greeted = false;

// ===== УМНОЕ ПРИВЕТСТВИЕ =====
bool sendGreetingOnBoot = false;

String MainMenu;
String SettingsMenu;
String WateringMenu;
String LightingMenu;
String WaterDurationMenu;
String LightDurationMenu;
String WateringTimeMenu;
String TimeEditMenu;
String ManualWaterMenu;

// ===== FORWARD DECLARATIONS =====
void initBotAndMenus();
void startAPMode();
void stopAPMode();
void tryConnectWiFiAsync();
void serialPortReading();
void GreetingSerial();
void deleteSystemData();
void blinkRx();
void recoverI2C();
void handleWiFiConnected();
void startManualWatering(int minutes);
void printResetReason();

// ===== СВЕТОДИОДНАЯ ИНДИКАЦИЯ =====
bool systemError = false;
unsigned long lastBlink = 0;
bool ledState = false;
bool rxBlinkActive = false;
unsigned long rxBlinkStart = 0;

// ===== ДИАГНОСТИКА ПЕРЕЗАГРУЗКИ =====
void printResetReason() {
  esp_reset_reason_t reason = esp_reset_reason();
  Serial.print("🔍 Причина перезагрузки: ");
  switch (reason) {
    case ESP_RST_POWERON:  Serial.println("Power-on (питание)"); break;
    case ESP_RST_SW:       Serial.println("Software (ESP.restart)"); break;
    case ESP_RST_PANIC:    Serial.println("Panic (краш/исключение)"); break;
    case ESP_RST_INT_WDT:  Serial.println("Interrupt WDT (зависание)"); break;
    case ESP_RST_TASK_WDT: Serial.println("Task WDT (loop() заблокирован)"); break;
    case ESP_RST_BROWNOUT: Serial.println("Brownout (просадка питания!)"); break;
    case ESP_RST_DEEPSLEEP: Serial.println("Deep sleep"); break;
    case ESP_RST_EXT:      Serial.println("External reset (кнопка)"); break;
    default:               Serial.println("Другая"); break;
  }
}

// ===== ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ =====

String getCurrentTime() {
  if (!rtcReady) return "00:00";
  DateTime now = rtc.now();
  char buf[16];
  sprintf(buf, "%02d:%02d", now.hour(), now.minute());
  return String(buf);
}

String getCurrentDateTime() {
  if (!rtcReady) return "Ошибка RTC";
  DateTime now = rtc.now();
  char buf[32];
  sprintf(buf, "%02d.%02d.%04d %02d:%02d:%02d",
    now.day(), now.month(), now.year(),
    now.hour(), now.minute(), now.second());
  return String(buf);
}

void initRTC() {
  Serial.println("🔧 initRTC: старт");

  // Шаг 1: rtc.begin() (внутри вызывает Wire.begin() с дефолтными пинами)
  if (!rtc.begin()) {
    Serial.println("❌ rtc.begin() failed — DS3231 не найден");
    rtcReady = false;
    return;
  }
  Serial.println("✅ rtc.begin() OK");

  // Шаг 2: ПЕРЕОПРЕДЕЛЯЕМ пины и скорость на НАШИ (21/22)
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  delay(50);

  // Шаг 3: проверяем адрес уже на наших пинах
  Wire.beginTransmission(0x68);
  if (Wire.endTransmission() != 0) {
    Serial.println("❌ DS3231 не отвечает по 0x68 после переустановки пинов");
    rtcReady = false;
    return;
  }
  Serial.println("✅ DS3231 отвечает на I2C (пины 21/22)");

  rtcReady = true;

  // Шаг 4: проверка потери питания
  if (rtc.lostPower()) {
    Serial.println("⚠️ RTC lost power — ставлю время сборки");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  // Шаг 5: санитарная проверка года — НЕ сбрасываем rtcReady, а правим время
  DateTime now = rtc.now();
  if (now.year() < 2024 || now.year() > 2100) {
    Serial.print("⚠️ RTC вернул год ");
    Serial.print(now.year());
    Serial.println(" — правлю на время сборки");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    now = rtc.now();
  }

  Serial.print("📅 Время RTC: ");
  Serial.println(getCurrentDateTime());
}

void recoverI2C() {
  Serial.println("🔄 Попытка восстановления шины I2C...");

  Wire.end();
  pinMode(SDA_PIN, OUTPUT);
  pinMode(SCL_PIN, OUTPUT);

  digitalWrite(SDA_PIN, HIGH);
  for (int i = 0; i < 9; i++) {
    digitalWrite(SCL_PIN, HIGH);
    delayMicroseconds(5);
    digitalWrite(SCL_PIN, LOW);
    delayMicroseconds(5);
  }

  digitalWrite(SDA_PIN, LOW);
  delayMicroseconds(5);
  digitalWrite(SCL_PIN, HIGH);
  delayMicroseconds(5);
  digitalWrite(SDA_PIN, HIGH);
  delayMicroseconds(5);

  pinMode(SDA_PIN, INPUT_PULLUP);
  pinMode(SCL_PIN, INPUT_PULLUP);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  delay(50);

  // НЕ вызываем rtc.begin() — он снова сбросит пины!
  Wire.beginTransmission(0x68);
  if (Wire.endTransmission() == 0) {
    rtcReady = true;
    Serial.println("✅ I2C восстановлен, RTC снова доступен");
    Serial.print("📅 Время RTC: ");
    Serial.println(getCurrentDateTime());
  } else {
    Serial.println("❌ DS3231 всё ещё не отвечает по адресу 0x68");
  }
}

bool setRtcTime(int year, int month, int day, int hour, int minute) {
  if (!rtcReady) return false;
  if (year < 2000 || year > 2099) return false;
  if (month < 1 || month > 12) return false;
  if (day < 1 || day > 31) return false;
  if (hour < 0 || hour > 23) return false;
  if (minute < 0 || minute > 59) return false;
  rtc.adjust(DateTime(year, month, day, hour, minute, 0));
  return true;
}

// ===== УВЕДОМЛЕНИЯ В VK =====
void notifyUser(const String& message) {
  if (bot == nullptr) return;
  preferences.begin("bot-config", true);
  long lastPeer = preferences.getLong("last_peer", 0);
  preferences.end();
  if (lastPeer == 0) {
    Serial.println("⚠️ Некуда уведомлять — пользователь не писал боту");
    return;
  }
  Serial.println("📢 Уведомление: " + message);
  bot->send(lastPeer, message);
}

// ===== СВЕТОДИОДНАЯ ИНДИКАЦИЯ =====

void blinkRx() {
  rxBlinkActive = true;
  rxBlinkStart = millis();
}

void updateStatusLED() {
  if (systemError) {
    digitalWrite(STATUS_LED_PIN, HIGH);
    ledState = true;
    return;
  }
  if (rxBlinkActive) {
    if (millis() - rxBlinkStart < 100) {
      digitalWrite(STATUS_LED_PIN, HIGH);
      ledState = true;
    } else {
      digitalWrite(STATUS_LED_PIN, LOW);
      ledState = false;
      rxBlinkActive = false;
    }
    return;
  }
  if (apMode) {
    if (millis() - lastBlink >= 1000) {
      lastBlink = millis();
      ledState = !ledState;
      digitalWrite(STATUS_LED_PIN, ledState);
    }
    return;
  }
  if (ledState) {
    digitalWrite(STATUS_LED_PIN, LOW);
    ledState = false;
  }
}

// ===== УПРАВЛЕНИЕ РЕЛЕ =====

void startLight(bool silent = false) {
  #if RELAY_ACTIVE_LOW
    digitalWrite(LIGHT_PIN, LOW);
  #else
    digitalWrite(LIGHT_PIN, HIGH);
  #endif
  lightState = true;
  lightStartTime = millis();
  if (!silent) notifyUser("💡 Свет включён!\n📅 " + getCurrentDateTime());
}

void stopLight(bool silent = false) {
  #if RELAY_ACTIVE_LOW
    digitalWrite(LIGHT_PIN, HIGH);
  #else
    digitalWrite(LIGHT_PIN, LOW);
  #endif
  lightState = false;
  lightStartTime = 0;
  if (!silent) notifyUser("✅💡 Свет выключен!\n📅 " + getCurrentDateTime());
}

void startPump(bool silent = false) {
  #if RELAY_ACTIVE_LOW
    digitalWrite(PUMP_PIN, LOW);
  #else
    digitalWrite(PUMP_PIN, HIGH);
  #endif
  pumpState = true;
  pumpStartTime = millis();
  if (!silent) notifyUser("💧 Полив начат!\n📅 " + getCurrentDateTime());
}

void stopPump(bool silent = false) {
  #if RELAY_ACTIVE_LOW
    digitalWrite(PUMP_PIN, HIGH);
  #else
    digitalWrite(PUMP_PIN, LOW);
  #endif
  pumpState = false;
  pumpStartTime = 0;
  manualWaterActive = false;
  manual_water_duration_value = 0;
  if (!silent) notifyUser("✅💧 Полив завершён!\n📅 " + getCurrentDateTime());
}

// ===== РУЧНОЙ ПОЛИВ =====
void startManualWatering(int minutes) {
  if (minutes <= 0) return;
  if (pumpState) {
    notifyUser("⚠️ Насос уже работает. Дождитесь окончания текущего полива.");
    return;
  }

  manualWaterActive = true;
  manual_water_duration_value = (unsigned long)minutes * 60000UL;
  startPump();

  notifyUser("💧 РУЧНОЙ ПОЛИВ НАЧАТ!\n"
             "⏱ Длительность: " + String(minutes) + " мин\n"
             "📅 " + getCurrentDateTime());
}

// ===== ПРОВЕРКА РАСПИСАНИЯ (БЕЗОПАСНАЯ ВЕРСИЯ) =====
void checkSchedule() {
  if (pumpState && pumpStartTime > 0) {
    unsigned long elapsed = millis() - pumpStartTime;

    unsigned long activeDuration = manualWaterActive
      ? manual_water_duration_value
      : (unsigned long)water_duration_value;

    if (elapsed > PUMP_ABSOLUTE_MAX_MS) {
      Serial.println("🚨 ПРЕДОХРАНИТЕЛЬ: насос работал дольше 30 мин — аварийное выключение!");
      stopPump(true);
      notifyUser("🚨 АВАРИЙНОЕ ОТКЛЮЧЕНИЕ НАСОСА!\nНасос работал более 30 минут.\nПроверьте систему.");
      return;
    }

    if (elapsed >= activeDuration) {
      if (manualWaterActive) {
        Serial.println("⏱ Ручной полив завершён — выключаю насос");
      } else {
        Serial.println("⏱ Таймер полива истёк — выключаю насос");
      }
      stopPump();
    }
  }

  if (lightState && lightStartTime > 0) {
    unsigned long elapsed = millis() - lightStartTime;
    if (elapsed >= (unsigned long)light_duration_value * 1000) {
      Serial.println("⏱ Таймер света истёк — выключаю свет");
      stopLight();
    }
  }

  if (!rtcReady) return;

  DateTime now = rtc.now();

  if (now.year() < 2024 || now.year() > 2100) {
    Serial.print("⚠️ RTC вернул некорректный год ");
    Serial.print(now.year());
    Serial.println(" — правлю из времени сборки");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    return;
  }

  String currentTime = getCurrentTime();

  if (light_duration_value > 0) {
    if (currentTime == lightTime && !lightState) startLight();
  }

  if (water_duration_value > 0 && !pumpState) {
    for (int i = 0; i < WATER_SLOTS; i++) {
      if (waterSlots[i].length() > 0 &&
          waterSlots[i] == currentTime &&
          millis() - lastWaterTriggerMs > 60000) {
        manualWaterActive = false;
        startPump();
        lastWaterTriggerMs = millis();
        break;
      }
    }
  }
}

String getSystemStatus() {
  String status;
  status.reserve(600);

  status += "📊 СТАТУС СИСТЕМЫ\n\n";
  status += "💧 ПОЛИВ:\n";
  status += "   • Времена: ";
  bool anyWaterSlot = false;
  for (int i = 0; i < WATER_SLOTS; i++) {
    if (waterSlots[i].length() > 0) {
      if (anyWaterSlot) status += ", ";
      status += waterSlots[i];
      anyWaterSlot = true;
    }
  }
  if (!anyWaterSlot) status += "не заданы";
  status += "\n";
  status += "   • Длительность: " + String(water_duration_value / 60000) + " мин\n";
  status += "   • Статус: " + String(pumpState ? "НАСОС ВКЛЮЧЕН" : "НАСОС ВЫКЛЮЧЕН");
  if (pumpState && manualWaterActive) {
    unsigned long remaining = (manual_water_duration_value - (millis() - pumpStartTime)) / 1000;
    status += " (ручной, осталось " + String(remaining) + " сек)";
  }
  status += "\n\n";

  status += "💡 СВЕТ:\n";
  status += "   • Время включения: " + lightTime + "\n";
  status += "   • Длительность: " + String(light_duration_value / 3600) + " ч\n";
  status += "   • Статус: " + String(lightState ? "ВКЛЮЧЕН" : "ВЫКЛЮЧЕН") + "\n\n";

  status += "📅 Время системы: " + getCurrentDateTime();
  return status;
}

String buildWaterSlotsText() {
  String s;
  s.reserve(200);
  s = "⏰ ВРЕМЕНА ПОЛИВА:\n\n";
  for (int i = 0; i < WATER_SLOTS; i++) {
    s += String(i + 1) + ". ";
    s += (waterSlots[i].length() == 0 ? "полив отсутствует" : waterSlots[i]);
    s += "\n";
  }
  s += "\nВыберите слот для редактирования.";
  return s;
}

void saveWaterSlots() {
  String packed;
  packed.reserve(40);
  for (int i = 0; i < WATER_SLOTS; i++) {
    if (i > 0) packed += ",";
    packed += waterSlots[i];
  }
  preferences.begin("bot-config", false);
  preferences.putString("water_slots", packed);
  preferences.end();
}

void loadWaterSlots() {
  preferences.begin("bot-config", true);
  String packed = preferences.getString("water_slots", "");
  preferences.end();
  for (int i = 0; i < WATER_SLOTS; i++) waterSlots[i] = "";
  if (packed.length() == 0) return;
  int start = 0, idx = 0;
  while (start <= packed.length() && idx < WATER_SLOTS) {
    int comma = packed.indexOf(',', start);
    if (comma < 0) comma = packed.length();
    waterSlots[idx++] = packed.substring(start, comma);
    start = comma + 1;
  }
}

// ===== ХЕЛПЕРЫ ПАРАМЕТРОВ =====
void saveWaterDuration(int val) {
  preferences.begin("bot-config", false);
  preferences.putInt("water_duration", val);
  preferences.end();
}
void saveLightDuration(int val) {
  preferences.begin("bot-config", false);
  preferences.putInt("light_duration", val);
  preferences.end();
}
void saveLightTime(const String& val) {
  preferences.begin("bot-config", false);
  preferences.putString("light_time", val);
  preferences.end();
}

// ===== HTML СТРАНИЦА =====

void handleRoot() {
  preferences.begin("bot-config", true);
  String s_ssid  = preferences.getString("ssid", "");
  String s_pass  = preferences.getString("password", "");
  String s_token = preferences.getString("token", "");
  String s_group = preferences.getString("group_id", "");
  preferences.end();

  String html = R"rawliteral(<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>ArtWat — Системные настройки</title>
<style>
    * { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
    :root { --purple: #8d369f; --purple-dark: #6d2880; --purple-light: #f4e8f7; --white: #ffffff; }
    html, body { margin: 0; padding: 0; }
    body { font-family: Arial, Helvetica, sans-serif; background: #d8dde3; color: #1a1a1a; min-height: 100vh; }
    .phone { max-width: 440px; margin: 0 auto; background: #fafafa; min-height: 100vh; display: flex; flex-direction: column; }
    .header { background: var(--purple); color: var(--white); padding: 26px 20px 22px; text-align: center; }
    .header .logo-text { font-size: 26px; font-weight: bold; letter-spacing: 0.3px; line-height: 1.2; }
    .header .subtitle { margin-top: 8px; font-size: 13px; opacity: 0.9; letter-spacing: 0.3px; }
    .container { padding: 20px; flex: 1; }
    .card { background: white; border-radius: 14px; padding: 22px 20px; margin-bottom: 16px; box-shadow: 0 2px 10px rgba(141, 54, 159, 0.08); border: 1px solid #f0e8f2; }
    .card h2 { margin: 0 0 4px; font-size: 17px; font-weight: bold; font-family: 'Arial Black', Arial, sans-serif; color: var(--purple); letter-spacing: -0.2px; }
    .card .card-sub { margin: 0 0 18px; font-size: 13px; color: #888; line-height: 1.4; }
    .field { margin-bottom: 16px; }
    .field label { display: block; font-size: 12px; color: var(--purple); text-transform: uppercase; letter-spacing: 0.8px; margin-bottom: 6px; font-weight: bold; }
    .field input[type="text"], .field input[type="password"], .field input[type="number"] { width: 100%; padding: 13px 14px; font-size: 14px; font-family: Arial, sans-serif; background: var(--purple-light); border: 1px solid #e5d0ea; border-radius: 10px; color: #1a1a1a; transition: border 0.15s, background 0.15s; }
    .field input:focus { outline: none; border-color: var(--purple); background: #fff; }
    .field input::placeholder { color: #b8a0c0; font-size: 13px; }
    .row { display: flex; gap: 8px; flex-wrap: wrap; }
    .row .field { flex: 1; min-width: 0; }
    .row .field.day { flex: 0 0 30%; } .row .field.month { flex: 0 0 30%; } .row .field.year { flex: 0 0 36%; }
    .row .field.hour { flex: 0 0 48%; } .row .field.minute { flex: 0 0 48%; }
    button, input[type="submit"] { width: 100%; padding: 15px; border: none; border-radius: 10px; font-size: 15px; font-weight: bold; font-family: Arial, sans-serif; cursor: pointer; transition: transform 0.1s, background 0.15s; letter-spacing: 0.2px; }
    button:active:not(:disabled), input[type="submit"]:active { transform: scale(0.98); }
    .btn-primary { background: var(--purple); color: var(--white); margin-top: 8px; }
    .btn-primary:active:not(:disabled) { background: var(--purple-dark); }
    .btn-primary:disabled { background: #cdb5d3; cursor: not-allowed; opacity: 0.7; }
    details { margin-top: 12px; border-radius: 10px; border: 1px solid #e8eaed; background: #f7f8fa; padding: 0; }
    details summary { padding: 12px 16px; font-size: 13px; font-weight: bold; color: var(--purple); cursor: pointer; user-select: none; list-style: none; display: flex; justify-content: space-between; align-items: center; }
    details summary::-webkit-details-marker { display: none; }
    details summary::after { content: '+'; font-size: 20px; color: var(--purple); line-height: 1; }
    details[open] summary::after { content: '−'; }
    details .details-body { padding: 0 16px 16px; font-size: 13px; line-height: 1.6; color: #555; }
    details .details-body ol { margin: 0; padding-left: 20px; }
    details .details-body ol li { margin-bottom: 6px; }
    details .details-body ol li:last-child { margin-bottom: 0; }
    details .details-body b { color: var(--purple); }
    .footer { text-align: center; padding: 16px; font-size: 11px; color: #aaa; }
    .file-input-wrap { position: relative; margin: 12px 0; }
    .file-input-wrap input[type="file"] { display: none; }
    .file-input-label { display: block; padding: 15px; text-align: center; background: white; color: var(--purple); border: 2px dashed #d9b8e0; border-radius: 10px; font-size: 14px; font-weight: bold; cursor: pointer; transition: background 0.15s, border-color 0.15s; }
    .file-input-label:active { background: var(--purple-light); border-color: var(--purple); }
    .file-name { margin-top: 8px; text-align: center; font-size: 12px; color: #888; font-family: Consolas, Monaco, monospace; word-break: break-all; }
    .progress-wrap { margin: 16px 0; display: none; }
    .progress-wrap.active { display: block; }
    .progress-bar-bg { width: 100%; height: 14px; background: var(--purple-light); border-radius: 7px; overflow: hidden; border: 1px solid #e5d0ea; }
    .progress-bar-fill { height: 100%; width: 0%; background: var(--purple); transition: width 0.2s ease; border-radius: 7px; }
    .progress-text { text-align: center; margin-top: 10px; font-size: 14px; font-weight: bold; color: var(--purple); }
    .status-block { margin-top: 14px; padding: 14px 16px; border-radius: 10px; font-size: 13px; line-height: 1.5; display: none; }
    .status-block.success { display: block; background: #ebf7eb; border: 1px solid #b8dfb8; color: #2d6a2d; }
    .status-block.error { display: block; background: #fdf2f2; border: 1px solid #f5d0d0; color: #7a2929; }
</style>
</head>
<body>
<div class="phone">
    <div class="header">
        <div class="logo-text">ArtWat — Настройка</div>
        <div class="subtitle">Системные параметры автополива</div>
    </div>
    <div class="container">
        <form action="/save" method="POST">
            <div class="card">
                <h2>Подключение к Wi-Fi</h2>
                <p class="card-sub">Данные вашей домашней сети, к которой подключится устройство</p>
                <div class="field">
                    <label for="ssid">Имя сети (SSID)</label>
                    <input type="text" id="ssid" name="ssid" required value="%%SSID%%" placeholder="Имя подключаемой сети Wi-Fi" autocomplete="off" autocorrect="off" autocapitalize="off" spellcheck="false">
                </div>
                <div class="field">
                    <label for="password">Пароль Wi-Fi</label>
                    <input type="password" id="password" name="password" value="%%PASS%%" placeholder="Пароль от домашнего Wi-Fi" autocomplete="new-password">
                </div>
            </div>
            <div class="card">
                <h2>Сообщество ВКонтакте</h2>
                <p class="card-sub">Данные, полученные в приложении ArtWat Setup</p>
                <div class="field">
                    <label for="token">Токен доступа (Access Token)</label>
                    <input type="text" id="token" name="token" required value="%%TOKEN%%" placeholder="vk1.a.XXXXXXXXX..." autocomplete="off" autocorrect="off" autocapitalize="off" spellcheck="false">
                </div>
                <div class="field">
                    <label for="group_id">ID сообщества (Group ID)</label>
                    <input type="text" id="group_id" name="group_id" required value="%%GROUP%%" placeholder="XXXXXXXXXX" autocomplete="off" autocorrect="off" autocapitalize="off" spellcheck="false">
                </div>
            </div>
            <div class="card">
                <h2>Системное время (RTC)</h2>
                <p class="card-sub">Установите текущее время для корректной работы расписания. Если оставить поля пустыми — время не изменится.</p>
                <div class="row">
                    <div class="field day"><label for="day">День</label><input type="number" id="day" name="day" min="1" max="31" placeholder="14"></div>
                    <div class="field month"><label for="month">Мес</label><input type="number" id="month" name="month" min="1" max="12" placeholder="10"></div>
                    <div class="field year"><label for="year">Год</label><input type="number" id="year" name="year" min="2000" max="2099" placeholder="2026"></div>
                </div>
                <div class="row">
                    <div class="field hour"><label for="hour">Час</label><input type="number" id="hour" name="hour" min="0" max="23" placeholder="15"></div>
                    <div class="field minute"><label for="minute">Мин</label><input type="number" id="minute" name="minute" min="0" max="59" placeholder="30"></div>
                </div>
            </div>
            <div class="card">
                <input type="submit" class="btn-primary" value="СОХРАНИТЬ И ПОДКЛЮЧИТЬСЯ">
            </div>
        </form>
        <details>
            <summary>Как получить токен и Group ID?</summary>
            <div class="details-body">
                <ol>
                    <li>Откройте <b>Mini App «ArtWat Setup»</b> в ВКонтакте.</li>
                    <li>Пройдите активацию: выберите сообщество, включите возможности ботов.</li>
                    <li>На экране <b>«ГОТОВО!»</b> нажмите кнопку <b>«СКОПИРОВАТЬ ВСЁ»</b>.</li>
                    <li>Вернитесь на эту страницу и вставьте данные в поля выше.</li>
                </ol>
            </div>
        </details>
        <details>
            <summary>Устройство не подключается к Wi-Fi</summary>
            <div class="details-body">
                <ol>
                    <li>Проверьте, что сеть работает в диапазоне <b>2.4 ГГц</b>.</li>
                    <li>Убедитесь, что имя сети (SSID) введено точно, с учётом регистра.</li>
                    <li>Если пароль содержит специальные символы — вводите их аккуратно.</li>
                    <li>Если сеть скрыта — устройство её не найдёт.</li>
                </ol>
            </div>
        </details>
        <details>
            <summary>Бот не отвечает в VK</summary>
            <div class="details-body">
                <ol>
                    <li>Проверьте, что в сообществе включены <b>«Возможности ботов»</b>.</li>
                    <li>Убедитесь, что написали боту хотя бы одно сообщение.</li>
                    <li>Если ничего не помогло — подключитесь по проводу, зайдите в Serial-монитор (115200), введите <b>/login</b> и далее <b>/check_parametrs</b>.</li>
                </ol>
            </div>
        </details>
        <details>
            <summary>Обновление прошивки</summary>
            <div class="details-body">
                <p style="margin-top: 0; font-size: 12px; color: #888;">
                    Скачайте актуальный файл <b>.bin</b> с нашего сообщества.
                </p>
                <div class="file-input-wrap">
                    <input type="file" id="fileInput" accept=".bin" onchange="onFileSelect(event)">
                    <label for="fileInput" class="file-input-label" id="fileLabel">ВЫБРАТЬ ФАЙЛ .BIN</label>
                    <div class="file-name" id="fileName"></div>
                </div>
                <button class="btn-primary" id="uploadBtn" onclick="uploadFirmware()" disabled style="margin-bottom: 0;">НАЧАТЬ ОБНОВЛЕНИЕ</button>
                <div class="progress-wrap" id="progressWrap">
                    <div class="progress-bar-bg"><div class="progress-bar-fill" id="progressFill"></div></div>
                    <div class="progress-text" id="progressText">0%</div>
                </div>
                <div class="status-block" id="statusBlock"></div>
            </div>
        </details>
    </div>
    <div class="footer">ArtWat © 2025 · Управление автополивом через VK</div>
</div>
<script>
    let selectedFile = null;
    const fileInput = document.getElementById('fileInput');
    const fileLabel = document.getElementById('fileLabel');
    const fileNameEl = document.getElementById('fileName');
    const uploadBtn = document.getElementById('uploadBtn');
    const progressWrap = document.getElementById('progressWrap');
    const progressFill = document.getElementById('progressFill');
    const progressText = document.getElementById('progressText');
    const statusBlock = document.getElementById('statusBlock');
    function formatBytes(bytes) {
        if (bytes < 1024) return bytes + ' Б';
        if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' КБ';
        return (bytes / (1024 * 1024)).toFixed(2) + ' МБ';
    }
    function onFileSelect(event) {
        const file = event.target.files[0];
        if (!file) { selectedFile = null; fileNameEl.textContent = ''; fileLabel.textContent = 'ВЫБРАТЬ ФАЙЛ .BIN'; uploadBtn.disabled = true; return; }
        selectedFile = file;
        fileNameEl.textContent = file.name + ' (' + formatBytes(file.size) + ')';
        fileLabel.textContent = 'ВЫБРАТЬ ДРУГОЙ ФАЙЛ';
        uploadBtn.disabled = false;
        statusBlock.className = 'status-block'; statusBlock.textContent = '';
        progressWrap.classList.remove('active'); progressFill.style.width = '0%'; progressText.textContent = '0%';
    }
    function uploadFirmware() {
        if (!selectedFile) return;
        uploadBtn.disabled = true; fileInput.disabled = true; fileLabel.style.pointerEvents = 'none'; fileLabel.style.opacity = '0.6';
        progressWrap.classList.add('active'); statusBlock.className = 'status-block'; statusBlock.textContent = '';
        progressFill.style.width = '0%'; progressText.textContent = '0%';
        const formData = new FormData();
        formData.append('update', selectedFile, selectedFile.name);
        const xhr = new XMLHttpRequest();
        xhr.open('POST', '/update', true);
        xhr.upload.onprogress = function (e) {
            if (e.lengthComputable) {
                const percent = Math.round((e.loaded / e.total) * 100);
                progressFill.style.width = percent + '%';
                progressText.textContent = percent + '%';
            }
        };
        xhr.onload = function () {
            if (xhr.status === 200) {
                progressFill.style.width = '100%'; progressText.textContent = '100%';
                statusBlock.className = 'status-block success';
                statusBlock.textContent = '✅ Прошивка успешно загружена. Устройство перезагружается...';
            } else {
                statusBlock.className = 'status-block error';
                statusBlock.textContent = '❌ Ошибка ' + xhr.status + ': ' + xhr.statusText;
                unlockUI();
            }
        };
        xhr.onerror = function () {
            statusBlock.className = 'status-block error';
            statusBlock.textContent = '❌ Не удалось отправить файл. Проверьте подключение к устройству.';
            unlockUI();
        };
        xhr.send(formData);
    }
    function unlockUI() { uploadBtn.disabled = false; fileInput.disabled = false; fileLabel.style.pointerEvents = ''; fileLabel.style.opacity = ''; }
</script>
</body>
</html>)rawliteral";

  html.replace("%%SSID%%",  s_ssid);
  html.replace("%%PASS%%",  s_pass);
  html.replace("%%TOKEN%%", s_token);
  html.replace("%%GROUP%%", s_group);

  server.send(200, "text/html; charset=utf-8", html);
}

void loadSettings() {
  preferences.begin("bot-config", true);
  String stored_ssid = preferences.getString("ssid", "");
  bool goto_config = preferences.getBool("goto_config", false);

  if (stored_ssid.length() == 0 || goto_config) {
    configMode = true;
  }

  lightTime = preferences.getString("light_time", "18:00");
  water_duration_value = preferences.getInt("water_duration", 0);
  light_duration_value = preferences.getInt("light_duration", 0);

  preferences.end();

  loadWaterSlots();
}

void handleSave() {
  if (server.hasArg("ssid") && server.hasArg("token") && server.hasArg("group_id")) {
    preferences.begin("bot-config", false);
    preferences.putString("ssid", server.arg("ssid"));
    preferences.putString("password", server.arg("password"));
    preferences.putString("token", server.arg("token"));
    preferences.putString("group_id", server.arg("group_id"));
    preferences.putBool("goto_config", false);
    preferences.putString("light_time", lightTime);
    preferences.putInt("water_duration", water_duration_value);
    preferences.putInt("light_duration", light_duration_value);
    preferences.putBool("boot_from_config", true);
    preferences.end();

    if (server.hasArg("year") && server.arg("year") != "") {
      setRtcTime(
        server.arg("year").toInt(),
        server.arg("month").toInt(),
        server.arg("day").toInt(),
        server.arg("hour").toInt(),
        server.arg("minute").toInt()
      );
    }

    server.send(200, "text/html; charset=UTF-8", 
      "<h2>✅ Настройки сохранены успешно!</h2>"
      "<p>🔄 Плата перезагружается для подключения...</p>"
    );
    delay(2000);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Bad Request");
  }
}

// ===== ОБНОВЛЕНИЕ ПРОШИВКИ =====

void handleUpdateFinish() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    delay(1000);
    ESP.restart();
}

void handleUpdateUpload() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("Update: %s\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            Serial.printf("Update Success: %u bytes\n", upload.totalSize);
        } else {
            Update.printError(Serial);
        }
    }
}

// ===== AP MODE =====

void startAPMode() {
  if (apMode) return;
  Serial.println("📡 Поднимаю точку доступа ArtWAT_system");

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  dnsServer.start(DNS_PORT, "*", apIP);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/update", HTTP_POST, handleUpdateFinish, handleUpdateUpload);

  server.onNotFound([]() {
    server.sendHeader("Location", String("http://") + apIP.toString(), true);
    server.send(302, "text/plain", "");
  });

  server.begin();
  apMode = true;
  systemError = false;

  Serial.print("📡 IP точки доступа: ");
  Serial.println(WiFi.softAPIP());
}

void stopAPMode() {
  if (!apMode) return;
  Serial.println("📡 Отключаю точку доступа");
  server.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  apMode = false;
  digitalWrite(STATUS_LED_PIN, LOW);
  ledState = false;
}

// ===== СБРОС ДАННЫХ =====

void deleteSystemData() {
  preferences.begin("bot-config", false);
  preferences.clear();
  preferences.end();
  delay(1500);
  Serial.flush();
  ESP.restart();
}

// ===== ОТКРЫТИЕ СЛОТА ПОЛИВА =====

void openTimeSlot(VKMessage& msg, int slot) {
  activeWaterSlot = slot;
  waitingForWaterTime = true;
  waitingUserId = msg.fromId;
  current_keyboard = "TimeEditMenu";

  String text;
  text.reserve(250);
  text = "⏰ ВРЕМЕННОЙ СЛОТ №" + String(slot + 1) + "\n\n";
  if (waterSlots[slot].length() == 0) {
    text += "Время не задано.\n";
  } else {
    text += "Текущее время: " + waterSlots[slot] + "\n";
  }
  text += "\nНапишите новое время в формате ЧЧ:ММ.\n\n";
  text += "Или нажмите «УБРАТЬ ПОЛИВ❌» чтобы насос не включался в данное время.";

  bot->sendWithKeyboard(msg.peerId, text, TimeEditMenu);
}

// ===== ИНИЦИАЛИЗАЦИЯ VK-БОТА И МЕНЮ =====

void initBotAndMenus() {
  static String s_main = createKeyboard(
    Button("СТАТУС📊", secondary),
    Line(),
    Button("НАСТРОЙКИ⚙️", secondary),
    Line(),
    Button("СИСТЕМНЫЕ НАСТРОЙКИ🔃", primary)
  );

  static String s_settings = createKeyboard(
    Button("ПОЛИВ💧", secondary),
    Line(),
    Button("СВЕТ💡", secondary),
    Line(),
    Button("НАЗАД↩️", primary)
  );

  static String s_watering = createKeyboard(
    Button("ДЛИТЕЛЬНОСТЬ💧⏱", secondary),
    Line(),
    Button("ВРЕМЯ ПОЛИВА💧🕐", secondary),
    Line(),
    Button("НАЧАТЬ ПОЛИВ💧", positive),
    Line(),
    Button("НАЗАД↩️", primary)
  );

  static String s_lighting = createKeyboard(
    Button("ДЛИТЕЛЬНОСТЬ💡⏱", secondary),
    Line(),
    Button("ВРЕМЯ ВКЛЮЧЕНИЯ💡🕐", secondary),
    Line(),
    Button("ВКЛЮЧИТЬ💡", positive),
    Button("ВЫКЛЮЧИТЬ💡", negative),
    Line(),
    Button("НАЗАД↩️", primary)
  );

  static String s_waterDur = createKeyboard(
    Button("1 минуту", secondary),
    Line(),
    Button("2 минуты", secondary),
    Line(),
    Button("3 минуты", secondary),
    Line(),
    Button("5 минут", secondary),
    Line(),
    Button("10 минут", secondary),
    Line(),
    Button("НАЗАД↩️", primary)
  );

  static String s_lightDur = createKeyboard(
    Button("2 часа", secondary),
    Line(),
    Button("4 часа", secondary),
    Line(),
    Button("6 часов", secondary),
    Line(),
    Button("8 часов", secondary),
    Line(),
    Button("12 часов", secondary),
    Line(),
    Button("НАЗАД↩️", primary)
  );

  static String s_waterTime = createKeyboard(
    Button("1 ВРЕМЯ", secondary),
    Button("2 ВРЕМЯ", secondary),
    Button("3 ВРЕМЯ", secondary),
    Line(),
    Button("ОЧИСТИТЬ ВСЕ🗑", negative),
    Line(),
    Button("НАЗАД↩️", primary)
  );

  static String s_timeEdit = createKeyboard(
    Button("УБРАТЬ ПОЛИВ❌", negative),
    Line(),
    Button("НАЗАД↩️", primary)
  );

  static String s_manual = createKeyboard(
    Button("1 мин", secondary),
    Button("5 мин", secondary),
    Line(),
    Button("СВОЁ ЗНАЧЕНИЕ", primary),
    Line(),
    Button("НАЗАД↩️", primary)
  );

  MainMenu = s_main;
  SettingsMenu = s_settings;
  WateringMenu = s_watering;
  LightingMenu = s_lighting;
  WaterDurationMenu = s_waterDur;
  LightDurationMenu = s_lightDur;
  WateringTimeMenu = s_waterTime;
  TimeEditMenu = s_timeEdit;
  ManualWaterMenu = s_manual;

  bot->onMessage("НАЧАТЬ ПОЛИВ💧", [](VKMessage& msg) {
    blinkRx();
    if (current_keyboard == "WateringMenu") {
      current_keyboard = "ManualWaterMenu";
      bot->sendWithKeyboard(msg.peerId,
        "💧 РУЧНОЙ ПОЛИВ\n\nВыберите длительность полива.\n"
        "Или нажмите «СВОЁ ЗНАЧЕНИЕ» чтобы ввести время в минутах.",
        ManualWaterMenu);
    }
  });

  bot->onMessage("1 мин", [](VKMessage& msg) {
    blinkRx();
    if (current_keyboard == "ManualWaterMenu") {
      current_keyboard = "WateringMenu";
      startManualWatering(1);
      bot->sendWithKeyboard(msg.peerId, "💧 Полив запущен на 1 минуту", WateringMenu);
    }
  });
  bot->onMessage("5 мин", [](VKMessage& msg) {
    blinkRx();
    if (current_keyboard == "ManualWaterMenu") {
      current_keyboard = "WateringMenu";
      startManualWatering(5);
      bot->sendWithKeyboard(msg.peerId, "💧 Полив запущен на 5 минут", WateringMenu);
    }
  });

  bot->onMessage("СВОЁ ЗНАЧЕНИЕ", [](VKMessage& msg) {
    blinkRx();
    if (current_keyboard == "ManualWaterMenu") {
      waitingForManualWaterTime = true;
      waitingManualUserId = msg.fromId;
      bot->send(msg.peerId,
        "✏️ ВВЕДИТЕ ДЛИТЕЛЬНОСТЬ ПОЛИВА\n\n"
        "Напишите число минут (от 1 до 30).\n"
        "Например: 7, 15, 25");
    }
  });

  bot->onMessage("СТАТУС📊", [](VKMessage& msg) {
    blinkRx();
    preferences.begin("bot-config", false);
    preferences.putLong("last_peer", msg.peerId);
    preferences.end();
    bot->send(msg.peerId, getSystemStatus());
  });

  bot->onMessage("НАСТРОЙКИ⚙️", [](VKMessage& msg) {
    blinkRx();
    current_keyboard = "SettingsMenu";
    bot->sendWithKeyboard(msg.peerId, "НАСТРОЙКИ⚙️\n\nВыберите раздел для редактирования параметров.", SettingsMenu);
  });

  bot->onMessage("ПОЛИВ💧", [](VKMessage& msg) {
    blinkRx();
    if (current_keyboard == "SettingsMenu") {
      current_keyboard = "WateringMenu";
      bot->sendWithKeyboard(msg.peerId, "ПОЛИВ💧\n\nРаздел для редактирования параметров полива", WateringMenu);
    }
  });

  bot->onMessage("ДЛИТЕЛЬНОСТЬ💧⏱", [](VKMessage& msg) {
    blinkRx();
    if (current_keyboard == "WateringMenu") {
      current_keyboard = "WaterDurationMenu";
      bot->sendWithKeyboard(msg.peerId, "⏱ ВЫБЕРИТЕ ДЛИТЕЛЬНОСТЬ ПОЛИВА", WaterDurationMenu);
    }
  });

  bot->onMessage("ВРЕМЯ ПОЛИВА💧🕐", [](VKMessage& msg) {
    blinkRx();
    if (current_keyboard == "WateringMenu") {
      current_keyboard = "WateringTimeMenu";
      bot->sendWithKeyboard(msg.peerId, buildWaterSlotsText(), WateringTimeMenu);
    }
  });

  bot->onMessage("1 ВРЕМЯ", [](VKMessage& msg) { blinkRx(); if (current_keyboard == "WateringTimeMenu") openTimeSlot(msg, 0); });
  bot->onMessage("2 ВРЕМЯ", [](VKMessage& msg) { blinkRx(); if (current_keyboard == "WateringTimeMenu") openTimeSlot(msg, 1); });
  bot->onMessage("3 ВРЕМЯ", [](VKMessage& msg) { blinkRx(); if (current_keyboard == "WateringTimeMenu") openTimeSlot(msg, 2); });

  bot->onMessage("ОЧИСТИТЬ ВСЕ🗑", [](VKMessage& msg) {
    blinkRx();
    if (current_keyboard == "WateringTimeMenu") {
      for (int i = 0; i < WATER_SLOTS; i++) waterSlots[i] = "";
      saveWaterSlots();
      bot->sendWithKeyboard(msg.peerId, "✅ Все времена очищены!\n\n" + buildWaterSlotsText(), WateringTimeMenu);
    }
  });

  bot->onMessage("УБРАТЬ ПОЛИВ❌", [](VKMessage& msg) {
    blinkRx();
    if (current_keyboard == "TimeEditMenu" && activeWaterSlot >= 0) {
      int slot = activeWaterSlot;
      waterSlots[slot] = "";
      saveWaterSlots();
      waitingForWaterTime = false;
      waitingUserId = 0;
      activeWaterSlot = -1;
      current_keyboard = "WateringTimeMenu";
      bot->sendWithKeyboard(msg.peerId, "✅ Слот очищен!\n\n" + buildWaterSlotsText(), WateringTimeMenu);
    }
  });

  bot->onMessage("СВЕТ💡", [](VKMessage& msg) {
    blinkRx();
    if (current_keyboard == "SettingsMenu") {
      current_keyboard = "LightingMenu";
      bot->sendWithKeyboard(msg.peerId, "СВЕТ💡\n\nРаздел редактирования параметров лампы.", LightingMenu);
    }
  });

  bot->onMessage("ДЛИТЕЛЬНОСТЬ💡⏱", [](VKMessage& msg) {
    blinkRx();
    if (current_keyboard == "LightingMenu") {
      current_keyboard = "LightDurationMenu";
      bot->sendWithKeyboard(msg.peerId, "⏱ ВЫБЕРИТЕ ДЛИТЕЛЬНОСТЬ РАБОТЫ ЛАМПЫ", LightDurationMenu);
    }
  });

  bot->onMessage("ВРЕМЯ ВКЛЮЧЕНИЯ💡🕐", [](VKMessage& msg) {
    blinkRx();
    if (current_keyboard == "LightingMenu") {
      waitingForLightTime = true;
      waitingLightUserId = msg.fromId;
      String message;
      message.reserve(200);
      message = "⏰ ВВЕДИТЕ ВРЕМЯ ВКЛЮЧЕНИЯ СВЕТА\n\n";
      message += "Напишите время в формате ЧЧ:ММ\n\n";
      message += "Текущее время включения: " + lightTime;
      bot->send(msg.peerId, message);
    }
  });

  bot->onMessage("ВКЛЮЧИТЬ💡", [](VKMessage& msg) {
    blinkRx();
    if (current_keyboard == "LightingMenu") {
      if (!lightState) { startLight(true); bot->send(msg.peerId, "💡 Свет включён вручную"); }
      else bot->send(msg.peerId, "💡 Свет уже включён");
    }
  });

  bot->onMessage("ВЫКЛЮЧИТЬ💡", [](VKMessage& msg) {
    blinkRx();
    if (current_keyboard == "LightingMenu") {
      if (lightState) { stopLight(true); bot->send(msg.peerId, "💡 Свет выключен вручную"); }
      else bot->send(msg.peerId, "💡 Свет уже выключен");
    }
  });

  bot->onMessage("1 минуту", [](VKMessage& msg) {
    blinkRx();
    if (current_keyboard == "WaterDurationMenu") {
      water_duration_value = 60000;
      saveWaterDuration(water_duration_value);
      bot->sendWithKeyboard(msg.peerId, "✅ Длительность полива: 1 минута", WateringMenu);
      current_keyboard = "WateringMenu";
    }
  });
  bot->onMessage("2 минуты", [](VKMessage& msg) {
    blinkRx();
    if (current_keyboard == "WaterDurationMenu") {
      water_duration_value = 120000;
      saveWaterDuration(water_duration_value);
      bot->sendWithKeyboard(msg.peerId, "✅ Длительность полива: 2 минуты", WateringMenu);
      current_keyboard = "WateringMenu";
    }
  });
  bot->onMessage("3 минуты", [](VKMessage& msg) {
    blinkRx();
    if (current_keyboard == "WaterDurationMenu") {
      water_duration_value = 180000;
      saveWaterDuration(water_duration_value);
      bot->sendWithKeyboard(msg.peerId, "✅ Длительность полива: 3 минуты", WateringMenu);
      current_keyboard = "WateringMenu";
    }
  });
  bot->onMessage("5 минут", [](VKMessage& msg) {
    blinkRx();
    if (current_keyboard == "WaterDurationMenu") {
      water_duration_value = 300000;
      saveWaterDuration(water_duration_value);
      bot->sendWithKeyboard(msg.peerId, "✅ Длительность полива: 5 минут", WateringMenu);
      current_keyboard = "WateringMenu";
    }
  });
  bot->onMessage("10 минут", [](VKMessage& msg) {
    blinkRx();
    if (current_keyboard == "WaterDurationMenu") {
      water_duration_value = 600000;
      saveWaterDuration(water_duration_value);
      bot->sendWithKeyboard(msg.peerId, "✅ Длительность полива: 10 минут", WateringMenu);
      current_keyboard = "WateringMenu";
    }
  });

  bot->onMessage("2 часа", [](VKMessage& msg) {
    blinkRx();
    if (current_keyboard == "LightDurationMenu") {
      light_duration_value = 7200;
      saveLightDuration(light_duration_value);
      bot->sendWithKeyboard(msg.peerId, "✅ Длительность света: 2 часа", LightingMenu);
      current_keyboard = "LightingMenu";
    }
  });
  bot->onMessage("4 часа", [](VKMessage& msg) {
    blinkRx();
    if (current_keyboard == "LightDurationMenu") {
      light_duration_value = 14400;
      saveLightDuration(light_duration_value);
      bot->sendWithKeyboard(msg.peerId, "✅ Длительность света: 4 часа", LightingMenu);
      current_keyboard = "LightingMenu";
    }
  });
  bot->onMessage("6 часов", [](VKMessage& msg) {
    blinkRx();
    if (current_keyboard == "LightDurationMenu") {
      light_duration_value = 21600;
      saveLightDuration(light_duration_value);
      bot->sendWithKeyboard(msg.peerId, "✅ Длительность света: 6 часов", LightingMenu);
      current_keyboard = "LightingMenu";
    }
  });
  bot->onMessage("8 часов", [](VKMessage& msg) {
    blinkRx();
    if (current_keyboard == "LightDurationMenu") {
      light_duration_value = 28800;
      saveLightDuration(light_duration_value);
      bot->sendWithKeyboard(msg.peerId, "✅ Длительность света: 8 часов", LightingMenu);
      current_keyboard = "LightingMenu";
    }
  });
  bot->onMessage("12 часов", [](VKMessage& msg) {
    blinkRx();
    if (current_keyboard == "LightDurationMenu") {
      light_duration_value = 43200;
      saveLightDuration(light_duration_value);
      bot->sendWithKeyboard(msg.peerId, "✅ Длительность света: 12 часов", LightingMenu);
      current_keyboard = "LightingMenu";
    }
  });

  bot->onMessage("СИСТЕМНЫЕ НАСТРОЙКИ🔃", [](VKMessage& msg) {
    blinkRx();
    bot->send(msg.peerId, "🔄 Вход в режим конфигурации...\n\nДанные сохранены!\nСеть ArtWAT_system запускается.\n\nПодключитесь к данной сети WiFi чтобы открыть системные настройки");
    delay(1000);
    preferences.begin("bot-config", false);
    preferences.putBool("goto_config", true);
    preferences.putBool("boot_from_config", false);
    preferences.end();
    ESP.restart(); 
  });

  bot->onMessage("НАЗАД↩️", [](VKMessage& msg) {
    blinkRx();
    if (current_keyboard == "ManualWaterMenu") {
      current_keyboard = "WateringMenu";
      bot->sendWithKeyboard(msg.peerId, "ПОЛИВ💧 НАСТРОЙКИ", WateringMenu);
    } else if (current_keyboard == "TimeEditMenu") {
      waitingForWaterTime = false; waitingUserId = 0; activeWaterSlot = -1;
      current_keyboard = "WateringTimeMenu";
      bot->sendWithKeyboard(msg.peerId, buildWaterSlotsText(), WateringTimeMenu);
    } else if (current_keyboard == "WateringTimeMenu") {
      current_keyboard = "WateringMenu";
      bot->sendWithKeyboard(msg.peerId, "ПОЛИВ💧 НАСТРОЙКИ", WateringMenu);
    } else if (current_keyboard == "WaterDurationMenu") {
      current_keyboard = "WateringMenu";
      bot->sendWithKeyboard(msg.peerId, "ПОЛИВ💧 НАСТРОЙКИ", WateringMenu);
    } else if (current_keyboard == "LightDurationMenu") {
      current_keyboard = "LightingMenu";
      bot->sendWithKeyboard(msg.peerId, "СВЕТ💡 НАСТРОЙКИ", LightingMenu);
    } else if (current_keyboard == "WateringMenu") {
      current_keyboard = "SettingsMenu";
      bot->sendWithKeyboard(msg.peerId, "НАСТРОЙКИ⚙️", SettingsMenu);
    } else if (current_keyboard == "LightingMenu") {
      current_keyboard = "SettingsMenu";
      bot->sendWithKeyboard(msg.peerId, "НАСТРОЙКИ⚙️", SettingsMenu);
    } else if (current_keyboard == "SettingsMenu") {
      current_keyboard = "MainMenu";
      bot->sendWithKeyboard(msg.peerId, "ГЛАВНОЕ МЕНЮ🌱", MainMenu);
    } else {
      current_keyboard = "MainMenu";
      bot->sendWithKeyboard(msg.peerId, "ГЛАВНОЕ МЕНЮ🌱", MainMenu);
    }
  });

  bot->onAnyMessage([](VKMessage& msg) {
    if (waitingForManualWaterTime && msg.fromId == waitingManualUserId) {
      blinkRx();
      int minutes = msg.text.toInt();
      if (minutes >= 1 && minutes <= 30) {
        waitingForManualWaterTime = false;
        waitingManualUserId = 0;
        current_keyboard = "WateringMenu";
        startManualWatering(minutes);
        bot->sendWithKeyboard(msg.peerId,
          "💧 Полив запущен на " + String(minutes) + " мин",
          WateringMenu);
      } else {
        bot->send(msg.peerId,
          "⚠️ Введите число от 1 до 30\n"
          "Например: 7, 15, 25");
      }
      return;
    }

    if (activeWaterSlot >= 0 && current_keyboard != "TimeEditMenu") {
      activeWaterSlot = -1; waitingForWaterTime = false; waitingUserId = 0;
    }

    if (waitingForWaterTime && msg.fromId == waitingUserId && activeWaterSlot >= 0) {
      blinkRx();
      String t = msg.text;
      if (t.length() == 5 && t[2] == ':') {
        int hh = t.substring(0, 2).toInt();
        int mm = t.substring(3, 5).toInt();
        if (hh >= 0 && hh < 24 && mm >= 0 && mm < 60) {
          bool duplicate = false;
          for (int i = 0; i < WATER_SLOTS; i++) {
            if (i != activeWaterSlot && waterSlots[i] == t) { duplicate = true; break; }
          }
          if (duplicate) { bot->send(msg.peerId, "⚠️ Это время уже стоит в другом слоте"); return; }
          int slot = activeWaterSlot;
          waterSlots[slot] = t;
          saveWaterSlots();
          waitingForWaterTime = false; waitingUserId = 0; activeWaterSlot = -1;
          current_keyboard = "WateringTimeMenu";
          bot->sendWithKeyboard(msg.peerId, "✅ Слот сохранён!\n\n" + buildWaterSlotsText(), WateringTimeMenu);
          return;
        }
      }
      bot->send(msg.peerId, "⚠️ Неверный формат времени.\n\nВведите время в формате ЧЧ:ММ\nНапример: 09:00, 14:20, 17:15");
      return;
    }

    if (waitingForLightTime && msg.fromId == waitingLightUserId) {
      blinkRx();
      String t = msg.text;
      if (t.length() == 5 && t[2] == ':') {
        int hh = t.substring(0, 2).toInt();
        int mm = t.substring(3, 5).toInt();
        if (hh >= 0 && hh < 24 && mm >= 0 && mm < 60) {
          waitingForLightTime = false; waitingLightUserId = 0;
          lightTime = t;
          saveLightTime(lightTime);
          current_keyboard = "LightingMenu";
          bot->sendWithKeyboard(msg.peerId, "✅ Время включения света установлено: " + lightTime, LightingMenu);
          return;
        }
      }
      bot->send(msg.peerId, "⚠️ Неверный формат времени.\n\nВведите время в формате ЧЧ:ММ\nНапример: 09:00, 14:20, 17:45");
      return;
    }

    blinkRx();
    bot->send(msg.peerId, "🤔 Неизвестная команда.\nИспользуйте кнопки меню или напишите СТАТУС📊");
  });
}

// ===== АСИНХРОННОЕ ПОДКЛЮЧЕНИЕ К WI-FI =====

void tryConnectWiFiAsync() {
  if (wifiConnecting) {
    Serial.println("📶 Уже идёт подключение...");
    return;
  }

  Serial.println("📶 Попытка подключения к Wi-Fi...");
  preferences.begin("bot-config", true);
  String ssid = preferences.getString("ssid", "");
  String password = preferences.getString("password", "");
  preferences.end();
  ssid.trim();
  password.trim();

  if (ssid.length() == 0) {
    Serial.println("⚠️ Wi-Fi не настроен.");
    lastWifiAttempt = millis();
    return;
  }

  Serial.print("SSID: ["); Serial.print(ssid); Serial.println("]");
  Serial.print("Heap: "); Serial.println(ESP.getFreeHeap());

  WiFi.disconnect(true, false);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());

  wifiConnecting = true;
  wifiConnectStart = millis();
  Serial.println("📶 Подключение запущено (асинхронно)");
}

void handleWiFiConnected() {
  Serial.println("\n✅ Wi-Fi подключен!");
  Serial.print("🌐 IP: ");
  Serial.println(WiFi.localIP());
  staConnected = true;
  systemError = false;
  wifiLostTime = 0;

  if (bot == nullptr) {
    preferences.begin("bot-config", true);
    String token = preferences.getString("token", "");
    String groupId = preferences.getString("group_id", "");
    long lastPeer = preferences.getLong("last_peer", 0);
    preferences.end();

    if (token.length() == 0 || groupId.length() == 0) {
      Serial.println("❌ Токен или Group ID пустые!");
      systemError = true;
      return;
    }

    bot = new VKassistent(token, groupId);
    bot->begin();
    initBotAndMenus();
    Serial.println("✅ Бот и меню инициализированы");
    systemError = false;

    if (lastPeer != 0) {
      if (sendGreetingOnBoot) {
        Serial.println("📤 Отправляю приветствие (Power-on или после сохранения настроек)");
        bot->sendWithKeyboard(lastPeer,
          "🌱 Система автополива ArtWAT запущена!\n\n"
          "📅 Системное время:\n" + getCurrentDateTime() + "\n\n"
          "Используйте кнопки для управления.",
          MainMenu
        );
      } else {
        Serial.println("ℹ️ Приветствие подавлено (не Power-on и не после сохранения настроек)");
      }
    } else {
      Serial.println("ℹ️ Приветствие не отправлено — пользователь ещё не писал боту");
    }
  }
}

// ===== SERIAL УПРАВЛЕНИЕ =====

void serialPortReading() {
  if (Serial.available() <= 0) return;

  String incomingString = Serial.readStringUntil('\n');
  incomingString.trim();
  if (incomingString.length() == 0) return;

  if (!serialAuthorized) {
    if (incomingString == "/login") {
      Serial.println("🔐 Введите пароль:");
    }
    else if (incomingString == SERIAL_PASSWORD) {
      serialAuthorized = true;
      Serial.println("✅ Доступ разрешён!");
      Serial.println("Введите /help для списка команд");
    }
    else {
      Serial.println("❌ Доступ запрещён!");
      Serial.println("Авторизируйтесь для использования комманд.");
      Serial.println();
      Serial.println("Введите /login для входа.");
    }
    return;
  }

  if (incomingString == "/delete_system_data") {
    waitingForDeleteConfirm = true;
    Serial.println("Вы удаляете все сохранённые системные данные");
    Serial.println("Вы УВЕРЕНЫ в том что сбрасываете систему до заводских настроек?");
    Serial.println("Введите команду /agree чтобы подтвердить действие, или");
    Serial.println("введите любую другую команду чтобы отменить сброс.");
  }
  else if (waitingForDeleteConfirm) {
    if (incomingString == "/agree") {
      waitingForDeleteConfirm = false;
      Serial.println("Ввод подтверждён ✅");
      Serial.println("Системные данные очищены...");
      Serial.println();
      Serial.println("Введите новые системные данные для подключения/работы");
      Serial.println("в режиме конфигурации системы.");
      Serial.println();
      Serial.println("Комманда режима конфигурации: /exit_on_config_mode.");
      deleteSystemData();
    } else {
      waitingForDeleteConfirm = false;
      Serial.println("Ввод отменём, в сбросе отказано ❌");
      Serial.println();
      Serial.println("Введите /help для списка команд");
    }
  }
  else if (incomingString == "/check_parametrs") {
    Serial.println("Выполняю проверку параметров...");
    Serial.println();
    Serial.println("=== ПАРАМЕТРЫ СИСТЕМЫ ===");
    Serial.print("RTC модуль: ");
    if (rtcReady) Serial.println("OK (" + getCurrentDateTime() + ")");
    else Serial.println("ОШИБКА (DS3231 не отвечает)");
    Serial.print("Режим Wi-Fi: ");
    if (apMode) Serial.println("AP Mode (точка доступа ArtWAT_system)");
    else if (wifiConnecting) Serial.println("Подключение...");
    else if (staConnected) Serial.println("STA Mode (подключен к роутеру)");
    else Serial.println("Автономный режим (нет сети)");
    Serial.print("IP-адрес: ");
    if (apMode) Serial.println(WiFi.softAPIP().toString());
    else if (staConnected) Serial.println(WiFi.localIP().toString());
    else Serial.println("—");
    Serial.print("Уровень сигнала: ");
    if (staConnected && !apMode) { Serial.print(WiFi.RSSI()); Serial.println(" dBm"); }
    else Serial.println("—");
    Serial.print("VK Бот: ");
    if (bot != nullptr) Serial.println("Инициализирован");
    else Serial.println("Ожидает подключения");
    preferences.begin("bot-config", true);
    String s_group = preferences.getString("group_id", "");
    preferences.end();
    Serial.print("Group ID: ");
    Serial.println(s_group.length() > 0 ? s_group : "не задан");

    Serial.println();
    Serial.print("Времена полива: ");
    bool anyWater = false;
    for (int i = 0; i < WATER_SLOTS; i++) {
      if (waterSlots[i].length() > 0) {
        if (anyWater) Serial.print(", ");
        Serial.print(waterSlots[i]);
        anyWater = true;
      }
    }
    if (!anyWater) Serial.print("не заданы");
    Serial.println();
    Serial.print("Длительность полива: "); Serial.print(String(water_duration_value / 60000)); Serial.println(" мин");
    Serial.print("Время включения света: "); Serial.println(lightTime);
    Serial.print("Длительность света: "); Serial.print(String(light_duration_value / 3600)); Serial.println(" ч");
    Serial.print("Насос: "); Serial.println(pumpState ? "ВКЛЮЧЕН" : "ВЫКЛЮЧЕН");
    Serial.print("Свет: ");  Serial.println(lightState ? "ВКЛЮЧЕН" : "ВЫКЛЮЧЕН");

    Serial.println();
    Serial.print("Свободно Heap: "); Serial.print(ESP.getFreeHeap() / 1024); Serial.println(" КБ");
    Serial.print("Макс. блок: "); Serial.print(ESP.getMaxAllocHeap() / 1024); Serial.println(" КБ");
    Serial.print("Версия прошивки: "); Serial.println("1.8.9 OTA (SmartBoot)");
    Serial.println("=========================");
  }
  else if (incomingString == "/save_params") {
    preferences.begin("bot-config", false);
    preferences.putString("light_time", lightTime);
    preferences.putInt("water_duration", water_duration_value);
    preferences.putInt("light_duration", light_duration_value);
    preferences.end();
    saveWaterSlots();
    Serial.println("✅ Параметры сохранены в память!");
    Serial.println("-----");
  }
  else if (incomingString == "/relay_test") {
    Serial.println("Выполняю проверку выходов...");
    for (int i = 0; i < 10; i++) {
      startPump(true); stopLight(true); delay(500);
      stopPump(true); startLight(true); delay(500);
    }
    stopPump(true); stopLight(true);
    Serial.println("✅ Тест реле завершен");
  }
  else if (incomingString == "/reboot_system") {
    Serial.println("Перезагрузка системы...");
    Serial.println("После перезагрузки нужно будет повторно авторизироваться.");
    Serial.println("Команда чтобы авторизироваться - /login");
    Serial.println("-----");
    delay(500);
    ESP.restart();
  }
  else if (incomingString == "/exit_on_config_mode") {
    Serial.println("Переход в режим конфигурации...");
    Serial.println("Плата перезагружается...");
    Serial.println("Подключитесь к сети Wi-Fi: ArtWAT_system");
    Serial.println("Откройте браузер и перейдите по адресу: http://192.168.4.1");
    Serial.println("-----");
    delay(1000);
    preferences.begin("bot-config", false);
    preferences.putBool("goto_config", true);
    preferences.putBool("boot_from_config", false);
    preferences.end();
    delay(500);
    ESP.restart();
  }
  else if (incomingString == "/logout") {
    serialAuthorized = false;
    Serial.println("Вы вышли из режима администратора");
    GreetingSerial();
  }
  else if (incomingString == "/help") {
    Serial.println("=== ДОСТУПНЫЕ КОМАНДЫ ===");
    Serial.println("/check_parametrs - показать все параметры");
    Serial.println("/save_params - сохранить параметры в память");
    Serial.println("/relay_test - тест реле");
    Serial.println("/pump_on - включить насос");
    Serial.println("/pump_off - выключить насос");
    Serial.println("/light_on - включить свет");
    Serial.println("/light_off - выключить свет");
    Serial.println("/reboot_system - перезагрузить систему");
    Serial.println("/exit_on_config_mode - режим конфигурации");
    Serial.println("/delete_system_data - сбросить до заводских настроек");
    Serial.println("/logout - выйти из режима администратора");
    Serial.println("/help - показать справку команд");
    Serial.println("=========================");
  }
  else if (incomingString == "/pump_on") { startPump(); Serial.println("Насос ВКЛЮЧЕН"); Serial.println("-----"); }
  else if (incomingString == "/pump_off") { stopPump(); Serial.println("Насос ВЫКЛЮЧЕН"); Serial.println("-----"); }
  else if (incomingString == "/light_on") { startLight(); Serial.println("Свет ВКЛЮЧЕН"); Serial.println("-----"); }
  else if (incomingString == "/light_off") { stopLight(); Serial.println("Свет ВЫКЛЮЧЕН"); Serial.println("-----"); }
  else {
    Serial.print("❌ Неизвестная команда: ");
    Serial.println(incomingString);
    Serial.println("Введите /help для списка команд");
    Serial.println("-----");
  }
}

void GreetingSerial() {
  Serial.println("=== ArtWat system ===");
  Serial.println("Войдите в режим разработчика чтобы системно управлять параметрами.");
  Serial.println("Команда чтобы авторизироваться - /login");
  Serial.println("=====================");
}

// ===== SETUP =====

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(50);
  delay(500);

  Serial.println();
  Serial.println("🔄 Загрузка системы ArtWAT 1.8.9...");

  printResetReason();
  esp_reset_reason_t reason = esp_reset_reason();

  preferences.begin("bot-config", true);
  bool bootFromConfig = preferences.getBool("boot_from_config", false);
  preferences.end();

  if (reason == ESP_RST_POWERON || bootFromConfig) {
    sendGreetingOnBoot = true;
  } else {
    sendGreetingOnBoot = false;
  }

  if (bootFromConfig) {
    preferences.begin("bot-config", false);
    preferences.putBool("boot_from_config", false);
    preferences.end();
  }

  if (sendGreetingOnBoot) {
    Serial.println("📤 Приветствие будет отправлено после подключения к VK");
  } else {
    Serial.println("🔇 Приветствие подавлено (не Power-on и не после сохранения настроек)");
  }

  pinMode(PUMP_PIN, OUTPUT);
  pinMode(LIGHT_PIN, OUTPUT);
  pinMode(STATUS_LED_PIN, OUTPUT);

  #if RELAY_ACTIVE_LOW
    digitalWrite(PUMP_PIN, HIGH);
    digitalWrite(LIGHT_PIN, HIGH);
  #else
    digitalWrite(PUMP_PIN, LOW);
    digitalWrite(LIGHT_PIN, LOW);
  #endif

  digitalWrite(STATUS_LED_PIN, LOW);
  ledState = false;

  initRTC();          // 🔧 FIX RTC — правильный порядок инициализации
  loadSettings();

  esp_task_wdt_deinit();
  esp_task_wdt_config_t wdt_config = {
      .timeout_ms = 90000,
      .idle_core_mask = 0,
      .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);

  if (configMode) {
    Serial.println("📡 Config Mode — AP Mode");
    startAPMode();
    wifiLostTime = millis();
  } else {
    preferences.begin("bot-config", true);
    String stored_ssid = preferences.getString("ssid", "");
    preferences.end();

    if (stored_ssid.length() == 0) {
      Serial.println("📡 Wi-Fi не настроен — AP Mode");
      startAPMode();
      wifiLostTime = millis();
    } else {
      Serial.println("📶 Wi-Fi настроен — асинхронное подключение");
      tryConnectWiFiAsync();

      unsigned long start = millis();
      while (millis() - start < 20000) {
        if (WiFi.status() == WL_CONNECTED) {
          wifiConnecting = false;
          handleWiFiConnected();
          break;
        }
        delay(200);
      }

      if (WiFi.status() != WL_CONNECTED) {
        wifiConnecting = false;
        Serial.println("📡 Не удалось подключиться — AP Mode");
        startAPMode();
        wifiLostTime = millis();
      }
    }
  }

  Serial.println("✅ setup() завершён");
}

// ===== LOOP =====

void loop() {
  esp_task_wdt_reset();

  updateStatusLED();
  serialPortReading();

  if (!rtcReady) {
    if (millis() - lastRtcRecoveryAttempt > RTC_RECOVERY_INTERVAL) {
      lastRtcRecoveryAttempt = millis();
      recoverI2C();   // 🔧 FIX RTC — без rtc.begin(), пины не сбиваются
    }
  }

  if (apMode) {
    dnsServer.processNextRequest();
    server.handleClient();
  }

  if (wifiConnecting) {
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnecting = false;
      handleWiFiConnected();
    } else if (millis() - wifiConnectStart > WIFI_CONNECT_TIMEOUT) {
      wifiConnecting = false;
      Serial.println("❌ Wi-Fi: таймаут 20 сек");
      systemError = true;
      lastWifiAttempt = millis();
    }
  }

  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 10000) {
    checkSchedule();   // 🔧 FIX RTC — не сбрасывает rtcReady
    lastCheck = millis();
  }

  if (WiFi.status() == WL_CONNECTED) {
    systemError = false;

    if (apMode) {
      stopAPMode();
      Serial.println("✅ Wi-Fi есть — AP Mode отключён");
    }

    if (!staConnected) {
      staConnected = true;
      wifiLostTime = 0;
      if (bot != nullptr) bot->begin();
      Serial.println("✅ STA Mode — бот активирован");
    }

    static unsigned long lastBotLoop = 0;
    if (bot != nullptr && millis() - lastBotLoop > 2000) {
      bot->loop();
      lastBotLoop = millis();
    }
  } else {
    if (staConnected) {
      staConnected = false;
      wifiLostTime = millis();
      Serial.println("⚠️ Wi-Fi потерян! Переход в автономный режим.");
    }

    if (!apMode) {
      startAPMode();
      Serial.println("📡 AP Mode активирован");
    } else {
      systemError = false;
    }

    if (!wifiConnecting && millis() - lastWifiAttempt > 300000) {
      Serial.println("🔄 Попытка переподключения...");
      tryConnectWiFiAsync();
    }
  }

  static unsigned long portOpenedTime = 0;
  if (!greeted) {
    if (portOpenedTime == 0) portOpenedTime = millis();
    if (millis() - portOpenedTime >= 200) {
      GreetingSerial();
      greeted = true;
    }
  }

  yield();
}
