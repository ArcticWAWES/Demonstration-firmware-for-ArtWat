// -------------------------------  СИСТЕМА АВТОПОЛИВА  -------------------------------
//                             ДЛЯ АВТОНОМНОГО ИСПОЛЬЗОВАНИЯ
//                              УПРАВЛЯЕМАЯ VK СООБЩЕСТВОМ
//                             НА БАЗЕ КАСТОМНОЙ БИБЛИОТЕКИ
//                                    "VKassistent"
//                                   by Arctic_WAWES
// 
//  Прошивка для серии моделей ArtWat (home edition) 
//  Firmwork version: 1.7.1 DEMO
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
//  - Световая индикация в лице всторенного на плату светодиода.
//  
// Распиновка модулей: 
// RTC DS3231 (I2C):
//    VCC  →  3.3V
//    GND  →  GND
//    SDA  →  GPIO 21
//    SCL  →  GPIO 22
//
//  Реле 1 (Насос):
//    VCC  →  5V
//    GND  →  GND
//    IN   →  GPIO 4    (АКТИВНОЕ НИЗКИМ: LOW = вкл, HIGH = выкл)
//
//  Реле 2 (Лампа):
//    VCC  →  5V
//    GND  →  GND
//    IN   →  GPIO 5    (АКТИВНОЕ НИЗКИМ: LOW = вкл, HIGH = выкл)
//
//  Светодиод индикации:
//    Встроенный на Wemos D1 R32 → GPIO 2
//

#include <VKassistent.h>
#include <Wire.h>
#include <RTClib.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_task_wdt.h>

const char* AP_SSID = "ArtWAT_system";
const char* AP_PASSWORD = "3468#2550";
const String SERIAL_PASSWORD = "3468#2550";

// I2C пины для DS3231
const int SDA_PIN = 21;
const int SCL_PIN = 22;

RTC_DS3231 rtc;
bool rtcReady = false;

const int PUMP_PIN = 4;
const int LIGHT_PIN = 5;

// ===== СВЕТОДИОД ИНДИКАЦИИ =====
const int STATUS_LED_PIN = 2;   // встроенный светодиод

// ===== ТИП РЕЛЕ =====
// true  — реле активные НИЗКИМ (LOW = вкл)
// false — реле активные ВЫСОКИМ (HIGH = вкл)
#define RELAY_ACTIVE_LOW true

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

// ===== ФЛАГИ SERIAL =====
bool waitingForDeleteConfirm = false;
bool serialAuthorized = false;
bool greeted = false;

String MainMenu;
String SettingsMenu;
String WateringMenu;
String LightingMenu;
String WaterDurationMenu;
String LightDurationMenu;
String WateringTimeMenu;
String TimeEditMenu;

// ===== FORWARD DECLARATIONS =====
void initBotAndMenus();
void startAPMode();
void stopAPMode();
void tryConnectWiFi();
void serialPortReading();
void GreetingSerial();
void deleteSystemData();

// ===== СВЕТОДИОДНАЯ ИНДИКАЦИЯ =====
bool systemError = false;
bool fastBlinkActive = false;
int  fastBlinksLeft = 0;
unsigned long lastBlink = 0;
bool ledState = false;

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
  Serial.println("🔧 initRTC: Wire.begin...");
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  delay(500);

  Serial.println("🔧 initRTC: проверяю адрес 0x68...");
  Wire.beginTransmission(0x68);
  if (Wire.endTransmission() != 0) {
    Serial.println("❌ DS3231 не отвечает по адресу 0x68");
    rtcReady = false;
    return;
  }
  Serial.println("✅ DS3231 отвечает на I2C");

  if (!rtc.begin()) {
    Serial.println("❌ RTClib не смог инициализировать DS3231");
    rtcReady = false;
    return;
  }
  Serial.println("✅ RTClib инициализирован");

  rtcReady = true;

  if (rtc.lostPower()) {
    Serial.println("⚠️ RTC потерял питание — ставлю время сборки");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  Serial.print("📅 Время RTC: ");
  Serial.println(getCurrentDateTime());
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
void notifyUser(String message) {
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

void triggerSuccessBlink() {
  fastBlinkActive = true;
  fastBlinksLeft = 10;
  lastBlink = millis();
  Serial.println("💡 LED: сигнал успешной инициализации");
}

void updateStatusLED() {
  if (fastBlinkActive) {
    if (millis() - lastBlink >= 100) {
      lastBlink = millis();
      ledState = !ledState;
      digitalWrite(STATUS_LED_PIN, ledState);
      if (!ledState) fastBlinksLeft--;
      if (fastBlinksLeft <= 0) {
        fastBlinkActive = false;
        digitalWrite(STATUS_LED_PIN, LOW);
      }
    }
    return;
  }

  if (systemError) {
    if (millis() - lastBlink >= 500) {
      lastBlink = millis();
      ledState = !ledState;
      digitalWrite(STATUS_LED_PIN, ledState);
    }
    return;
  }

  if (apMode) {
    if (!ledState) {
      digitalWrite(STATUS_LED_PIN, HIGH);
      ledState = true;
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

  if (!silent) {
    notifyUser("💡 Свет включён!\n📅 " + getCurrentDateTime());
  }
}

void stopLight(bool silent = false) {
  #if RELAY_ACTIVE_LOW
    digitalWrite(LIGHT_PIN, HIGH);
  #else
    digitalWrite(LIGHT_PIN, LOW);
  #endif
  lightState = false;
  lightStartTime = 0;

  if (!silent) {
    notifyUser("✅💡 Свет выключен!\n📅 " + getCurrentDateTime());
  }
}

void startPump(bool silent = false) {
  #if RELAY_ACTIVE_LOW
    digitalWrite(PUMP_PIN, LOW);
  #else
    digitalWrite(PUMP_PIN, HIGH);
  #endif
  pumpState = true;
  pumpStartTime = millis();

  if (!silent) {
    notifyUser("💧 Полив начат!\n📅 " + getCurrentDateTime());
  }
}

void stopPump(bool silent = false) {
  #if RELAY_ACTIVE_LOW
    digitalWrite(PUMP_PIN, HIGH);
  #else
    digitalWrite(PUMP_PIN, LOW);
  #endif
  pumpState = false;
  pumpStartTime = 0;

  if (!silent) {
    notifyUser("✅💧 Полив завершён!\n📅 " + getCurrentDateTime());
  }
}

void checkSchedule() {
  if (!rtcReady) return;

  String currentTime = getCurrentTime();

  // --- СВЕТ (одно время) ---
  if (light_duration_value > 0) {
    if (currentTime == lightTime && !lightState) startLight();
    if (lightState && lightStartTime > 0) {
      unsigned long elapsed = millis() - lightStartTime;
      if (elapsed >= (unsigned long)light_duration_value * 1000) stopLight();
    }
  }

  // --- ПОЛИВ (по слотам) ---
  if (water_duration_value > 0 && !pumpState) {
    for (int i = 0; i < WATER_SLOTS; i++) {
      if (waterSlots[i].length() > 0 &&
          waterSlots[i] == currentTime &&
          millis() - lastWaterTriggerMs > 60000) {
        startPump();
        lastWaterTriggerMs = millis();
        break;
      }
    }
  }
  if (pumpState && pumpStartTime > 0) {
    unsigned long elapsed = millis() - pumpStartTime;
    if (elapsed >= (unsigned long)water_duration_value) stopPump();
  }
}

String getSystemStatus() {
  String status = "📊 СТАТУС СИСТЕМЫ\n\n";

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
  status += "   • Статус: " + String(pumpState ? "НАСОС ВКЛЮЧЕН" : "НАСОС ВЫКЛЮЧЕН") + "\n\n";

  status += "💡 СВЕТ:\n";
  status += "   • Время включения: " + lightTime + "\n";
  status += "   • Длительность: " + String(light_duration_value / 3600) + " ч\n";
  status += "   • Статус: " + String(lightState ? "ВКЛЮЧЕН" : "ВЫКЛЮЧЕН") + "\n\n";

  status += "📅 Время системы: " + getCurrentDateTime();
  return status;
}

// ===== СЛОТЫ ВРЕМЕНИ ПОЛИВА =====

String buildWaterSlotsText() {
  String s = "⏰ ВРЕМЕНА ПОЛИВА:\n\n";
  for (int i = 0; i < WATER_SLOTS; i++) {
    s += String(i + 1) + ". ";
    s += (waterSlots[i].length() == 0 ? "полив отсутствует" : waterSlots[i]);
    s += "\n";
  }
  s += "\nВыберите слот для редактирования.";
  return s;
}

void saveWaterSlots() {
  String packed = "";
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

// ===== HTML СТРАНИЦА =====

// ===== HTML СТРАНИЦА =====

void handleRoot() {
  preferences.begin("bot-config", true);
  String s_ssid  = preferences.getString("ssid", "");
  String s_pass  = preferences.getString("password", "");
  String s_token = preferences.getString("token", "");
  String s_group = preferences.getString("group_id", "");
  preferences.end();

  // Сырая строка — HTML можно редактировать как обычно,
  // без экранирования кавычек и переводов строк.
  // Плейсхолдеры %%SSID%% и т.д. заменяются на сохранённые значения.
  String html = R"rawliteral(<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>ArtWat — Системные настройки</title>
<style>
    * { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }

    :root {
        --purple: #8d369f;
        --purple-dark: #6d2880;
        --purple-light: #f4e8f7;
        --white: #ffffff;
    }

    html, body { margin: 0; padding: 0; }

    body {
        font-family: Arial, Helvetica, sans-serif;
        background: #d8dde3;
        color: #1a1a1a;
        min-height: 100vh;
    }

    .phone {
        max-width: 440px;
        margin: 0 auto;
        background: #fafafa;
        min-height: 100vh;
        display: flex;
        flex-direction: column;
    }

    .header {
        background: var(--purple);
        color: var(--white);
        padding: 26px 20px 22px;
        text-align: center;
    }

    .header .logo-text {
        font-family: Arial, Helvetica, sans-serif;
        font-size: 26px;
        font-weight: bold;
        color: var(--white);
        letter-spacing: 0.3px;
        line-height: 1.2;
    }

    .header .subtitle {
        margin-top: 8px;
        font-size: 13px;
        opacity: 0.9;
        letter-spacing: 0.3px;
    }

    .container { padding: 20px; flex: 1; }

    .card {
        background: white;
        border-radius: 14px;
        padding: 22px 20px;
        margin-bottom: 16px;
        box-shadow: 0 2px 10px rgba(141, 54, 159, 0.08);
        border: 1px solid #f0e8f2;
    }

    .card h2 {
        margin: 0 0 4px;
        font-size: 17px;
        font-weight: bold;
        font-family: 'Arial Black', Arial, sans-serif;
        color: var(--purple);
        letter-spacing: -0.2px;
    }

    .card .card-sub {
        margin: 0 0 18px;
        font-size: 13px;
        color: #888;
        line-height: 1.4;
    }

    .field { margin-bottom: 16px; }

    .field label {
        display: block;
        font-size: 12px;
        color: var(--purple);
        text-transform: uppercase;
        letter-spacing: 0.8px;
        margin-bottom: 6px;
        font-weight: bold;
    }

    .field input[type="text"],
    .field input[type="password"],
    .field input[type="number"] {
        width: 100%;
        padding: 13px 14px;
        font-size: 14px;
        font-family: Arial, sans-serif;
        background: var(--purple-light);
        border: 1px solid #e5d0ea;
        border-radius: 10px;
        color: #1a1a1a;
        transition: border 0.15s, background 0.15s;
    }

    .field input:focus {
        outline: none;
        border-color: var(--purple);
        background: #fff;
    }

    .field input::placeholder {
        color: #b8a0c0;
        font-size: 13px;
    }

    .row { display: flex; gap: 8px; flex-wrap: wrap; }

    .row .field { flex: 1; min-width: 0; }
    .row .field.day   { flex: 0 0 30%; }
    .row .field.month { flex: 0 0 30%; }
    .row .field.year  { flex: 0 0 36%; }
    .row .field.hour   { flex: 0 0 48%; }
    .row .field.minute { flex: 0 0 48%; }

    button, input[type="submit"] {
        width: 100%;
        padding: 15px;
        border: none;
        border-radius: 10px;
        font-size: 15px;
        font-weight: bold;
        font-family: Arial, sans-serif;
        cursor: pointer;
        transition: transform 0.1s, background 0.15s;
        letter-spacing: 0.2px;
    }

    button:active, input[type="submit"]:active { transform: scale(0.98); }

    .btn-primary {
        background: var(--purple);
        color: var(--white);
        margin-top: 8px;
    }

    .btn-primary:active { background: var(--purple-dark); }

    details {
        margin-top: 12px;
        border-radius: 10px;
        border: 1px solid #e8eaed;
        background: #f7f8fa;
        padding: 0;
    }

    details summary {
        padding: 12px 16px;
        font-size: 13px;
        font-weight: bold;
        color: var(--purple);
        cursor: pointer;
        user-select: none;
        list-style: none;
        display: flex;
        justify-content: space-between;
        align-items: center;
    }

    details summary::-webkit-details-marker { display: none; }

    details summary::after {
        content: '+';
        font-size: 20px;
        color: var(--purple);
        line-height: 1;
    }

    details[open] summary::after { content: '−'; }

    details .details-body {
        padding: 0 16px 16px;
        font-size: 13px;
        line-height: 1.6;
        color: #555;
    }

    details .details-body ol { margin: 0; padding-left: 20px; }
    details .details-body ol li { margin-bottom: 6px; }
    details .details-body ol li:last-child { margin-bottom: 0; }
    details .details-body b { color: var(--purple); }

    .footer {
        text-align: center;
        padding: 16px;
        font-size: 11px;
        color: #aaa;
    }
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
                    <input type="text" id="ssid" name="ssid" required
                           value="%%SSID%%"
                           placeholder="Имя подключаемой сети Wi-Fi"
                           autocomplete="off" autocorrect="off" autocapitalize="off" spellcheck="false">
                </div>

                <div class="field">
                    <label for="password">Пароль Wi-Fi</label>
                    <input type="password" id="password" name="password"
                           value="%%PASS%%"
                           placeholder="Пароль от домашнего Wi-Fi"
                           autocomplete="new-password">
                </div>
            </div>

            <div class="card">
                <h2>Сообщество ВКонтакте</h2>
                <p class="card-sub">Данные, полученные в приложении ArtWat Setup</p>

                <div class="field">
                    <label for="token">Токен доступа (Access Token)</label>
                    <input type="text" id="token" name="token" required
                           value="%%TOKEN%%"
                           placeholder="vk1.a.XXXXXXXXX..."
                           autocomplete="off" autocorrect="off" autocapitalize="off" spellcheck="false">
                </div>

                <div class="field">
                    <label for="group_id">ID сообщества (Group ID)</label>
                    <input type="text" id="group_id" name="group_id" required
                           value="%%GROUP%%"
                           placeholder="XXXXXXXXXX"
                           autocomplete="off" autocorrect="off" autocapitalize="off" spellcheck="false">
                </div>
            </div>

            <div class="card">
                <h2>Системное время (RTC)</h2>
                <p class="card-sub">Установите текущее время для корректной работы расписания. Если оставить поля пустыми — время не изменится.</p>

                <div class="row">
                    <div class="field day">
                        <label for="day">День</label>
                        <input type="number" id="day" name="day" min="1" max="31" placeholder="14">
                    </div>
                    <div class="field month">
                        <label for="month">Мес</label>
                        <input type="number" id="month" name="month" min="1" max="12" placeholder="10">
                    </div>
                    <div class="field year">
                        <label for="year">Год</label>
                        <input type="number" id="year" name="year" min="2000" max="2099" placeholder="2026">
                    </div>
                </div>

                <div class="row">
                    <div class="field hour">
                        <label for="hour">Час</label>
                        <input type="number" id="hour" name="hour" min="0" max="23" placeholder="15">
                    </div>
                    <div class="field minute">
                        <label for="minute">Мин</label>
                        <input type="number" id="minute" name="minute" min="0" max="59" placeholder="30">
                    </div>
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
                <p style="margin: 12px 0 0; font-size: 12px; color: #888;">
                    Если у вас ещё нет сообщества ВКонтакте — создайте его заранее
                    в приложении VK: раздел «Сообщества» → кнопка «СОЗДАТЬ».
                </p>
            </div>
        </details>

        <details>
            <summary>Устройство не подключается к Wi-Fi</summary>
            <div class="details-body">
                <ol>
                    <li>Проверьте, что сеть работает в диапазоне <b>2.4 ГГц</b>
                        (устройство не поддерживает 5 ГГц).</li>
                    <li>Убедитесь, что имя сети (SSID) введено точно, с учётом регистра.</li>
                    <li>Если пароль содержит специальные символы — вводите их аккуратно.</li>
                    <li>Если сеть скрыта — устройство её не найдёт, нужно временно
                        сделать её видимой.</li>
                </ol>
            </div>
        </details>

        <details>
            <summary>Бот не отвечает в VK</summary>
            <div class="details-body">
                <ol>
                    <li>Проверьте, что в сообществе включены <b>«Возможности ботов»</b>:
                        Управление → Сообщения → Настройки для бота.</li>
                    <li>Убедитесь, что написали боту хотя бы одно сообщение — так
                        устройство запомнит ваш peer_id.</li>
                    <li>Если ничего не помогло — подключитесь по проводу к микроконтроллеру,
                        зайдите в Serial-монитор (115200 бод), войдите в режим разработчика,
                        введя персональный пароль системы (<b>/login</b>), и далее передайте
                        команду <b>/check_parametrs</b> для диагностики устройства.</li>
                </ol>
            </div>
        </details>

    </div>

    <div class="footer">ArtWat © 2025 · Управление автополивом через VK</div>

</div>

</body>
</html>)rawliteral";

  // Подстановка сохранённых значений в поля формы
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

  String text = "⏰ ВРЕМЕННОЙ СЛОТ №" + String(slot + 1) + "\n\n";
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
  MainMenu = createKeyboard(
    Button("СТАТУС📊", secondary),
    Line(),
    Button("НАСТРОЙКИ⚙️", secondary),
    Line(),
    Button("СИСТЕМНЫЕ НАСТРОЙКИ🔃", primary)
  );

  SettingsMenu = createKeyboard(
    Button("ПОЛИВ💧", secondary),
    Line(),
    Button("СВЕТ💡", secondary),
    Line(),
    Button("НАЗАД↩️", primary)
  );

  WateringMenu = createKeyboard(
    Button("ДЛИТЕЛЬНОСТЬ💧⏱", secondary),
    Line(),
    Button("ВРЕМЯ ПОЛИВА💧🕐", secondary),
    Line(),
    Button("НАЗАД↩️", primary)
  );

  LightingMenu = createKeyboard(
    Button("ДЛИТЕЛЬНОСТЬ💡⏱", secondary),
    Line(),
    Button("ВРЕМЯ ВКЛЮЧЕНИЯ💡🕐", secondary),
    Line(),
    Button("ВКЛЮЧИТЬ💡", positive),
    Button("ВЫКЛЮЧИТЬ💡", negative),
    Line(),
    Button("НАЗАД↩️", primary)
  );

  WaterDurationMenu = createKeyboard(
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

  LightDurationMenu = createKeyboard(
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

  WateringTimeMenu = createKeyboard(
    Button("1 ВРЕМЯ", secondary),
    Button("2 ВРЕМЯ", secondary),
    Button("3 ВРЕМЯ", secondary),
    Line(),
    Button("ОЧИСТИТЬ ВСЕ🗑", negative),
    Line(),
    Button("НАЗАД↩️", primary)
  );

  TimeEditMenu = createKeyboard(
    Button("УБРАТЬ ПОЛИВ❌", negative),
    Line(),
    Button("НАЗАД↩️", primary)
  );

  // ===== ОСНОВНЫЕ КОМАНДЫ =====

  bot->onMessage("СТАТУС📊", [](VKMessage& msg) {
    preferences.begin("bot-config", false);
    preferences.putLong("last_peer", msg.peerId);
    preferences.end();

    bot->send(msg.peerId, getSystemStatus());
  });

  bot->onMessage("НАСТРОЙКИ⚙️", [](VKMessage& msg) {
    current_keyboard = "SettingsMenu";
    bot->sendWithKeyboard(msg.peerId, "НАСТРОЙКИ⚙️\n\nВыберите раздел для редактирования параметров.", SettingsMenu);
  });

  bot->onMessage("ПОЛИВ💧", [](VKMessage& msg) {
    if (current_keyboard == "SettingsMenu") {
      current_keyboard = "WateringMenu";
      bot->sendWithKeyboard(msg.peerId, "ПОЛИВ💧\n\nРаздел для редактирования параметров полива", WateringMenu);
    }
  });

  bot->onMessage("ДЛИТЕЛЬНОСТЬ💧⏱", [](VKMessage& msg) {
    if (current_keyboard == "WateringMenu") {
      current_keyboard = "WaterDurationMenu";
      bot->sendWithKeyboard(msg.peerId, "⏱ ВЫБЕРИТЕ ДЛИТЕЛЬНОСТЬ ПОЛИВА", WaterDurationMenu);
    }
  });

  bot->onMessage("ВРЕМЯ ПОЛИВА💧🕐", [](VKMessage& msg) {
    if (current_keyboard == "WateringMenu") {
      current_keyboard = "WateringTimeMenu";
      bot->sendWithKeyboard(msg.peerId, buildWaterSlotsText(), WateringTimeMenu);
    }
  });

  bot->onMessage("1 ВРЕМЯ", [](VKMessage& msg) {
    if (current_keyboard == "WateringTimeMenu") openTimeSlot(msg, 0);
  });

  bot->onMessage("2 ВРЕМЯ", [](VKMessage& msg) {
    if (current_keyboard == "WateringTimeMenu") openTimeSlot(msg, 1);
  });

  bot->onMessage("3 ВРЕМЯ", [](VKMessage& msg) {
    if (current_keyboard == "WateringTimeMenu") openTimeSlot(msg, 2);
  });

  bot->onMessage("ОЧИСТИТЬ ВСЕ🗑", [](VKMessage& msg) {
    if (current_keyboard == "WateringTimeMenu") {
      for (int i = 0; i < WATER_SLOTS; i++) waterSlots[i] = "";
      saveWaterSlots();
      bot->sendWithKeyboard(msg.peerId,
        "✅ Все времена очищены!\n\n" + buildWaterSlotsText(), WateringTimeMenu);
    }
  });

  bot->onMessage("УБРАТЬ ПОЛИВ❌", [](VKMessage& msg) {
    if (current_keyboard == "TimeEditMenu" && activeWaterSlot >= 0) {
      int slot = activeWaterSlot;
      waterSlots[slot] = "";
      saveWaterSlots();

      waitingForWaterTime = false;
      waitingUserId = 0;
      activeWaterSlot = -1;

      current_keyboard = "WateringTimeMenu";
      bot->sendWithKeyboard(msg.peerId,
        "✅ Слот очищен!\n\n" + buildWaterSlotsText(),
        WateringTimeMenu);
    }
  });

  bot->onMessage("СВЕТ💡", [](VKMessage& msg) {
    if (current_keyboard == "SettingsMenu") {
      current_keyboard = "LightingMenu";
      bot->sendWithKeyboard(msg.peerId, "СВЕТ💡\n\nРаздел редактирования параметров лампы.", LightingMenu);
    }
  });

  bot->onMessage("ДЛИТЕЛЬНОСТЬ💡⏱", [](VKMessage& msg) {
    if (current_keyboard == "LightingMenu") {
      current_keyboard = "LightDurationMenu";
      bot->sendWithKeyboard(msg.peerId, "⏱ ВЫБЕРИТЕ ДЛИТЕЛЬНОСТЬ РАБОТЫ ЛАМПЫ", LightDurationMenu);
    }
  });

  bot->onMessage("ВРЕМЯ ВКЛЮЧЕНИЯ💡🕐", [](VKMessage& msg) {
    if (current_keyboard == "LightingMenu") {
      waitingForLightTime = true;
      waitingLightUserId = msg.fromId;

      String message = "⏰ ВВЕДИТЕ ВРЕМЯ ВКЛЮЧЕНИЯ СВЕТА\n\n";
      message += "Напишите время в формате ЧЧ:ММ\n\n";
      message += "Текущее время включения: " + lightTime;

      bot->send(msg.peerId, message);
    }
  });

  bot->onMessage("ВКЛЮЧИТЬ💡", [](VKMessage& msg) {
    if (current_keyboard == "LightingMenu") {
      if (!lightState) {
        startLight(true);
        bot->send(msg.peerId, "💡 Свет включён вручную");
      } else {
        bot->send(msg.peerId, "💡 Свет уже включён");
      }
    }
  });

  bot->onMessage("ВЫКЛЮЧИТЬ💡", [](VKMessage& msg) {
    if (current_keyboard == "LightingMenu") {
      if (lightState) {
        stopLight(true);
        bot->send(msg.peerId, "💡 Свет выключен вручную");
      } else {
        bot->send(msg.peerId, "💡 Свет уже выключен");
      }
    }
  });

  bot->onMessage("1 минуту", [](VKMessage& msg) {
    if (current_keyboard == "WaterDurationMenu") {
      water_duration_value = 60000;
      preferences.begin("bot-config", false);
      preferences.putInt("water_duration", water_duration_value);
      preferences.end();
      bot->sendWithKeyboard(msg.peerId, "✅ Длительность полива: 1 минута", WateringMenu);
      current_keyboard = "WateringMenu";
    }
  });

  bot->onMessage("2 минуты", [](VKMessage& msg) {
    if (current_keyboard == "WaterDurationMenu") {
      water_duration_value = 120000;
      preferences.begin("bot-config", false);
      preferences.putInt("water_duration", water_duration_value);
      preferences.end();
      bot->sendWithKeyboard(msg.peerId, "✅ Длительность полива: 2 минуты", WateringMenu);
      current_keyboard = "WateringMenu";
    }
  });

  bot->onMessage("3 минуты", [](VKMessage& msg) {
    if (current_keyboard == "WaterDurationMenu") {
      water_duration_value = 180000;
      preferences.begin("bot-config", false);
      preferences.putInt("water_duration", water_duration_value);
      preferences.end();
      bot->sendWithKeyboard(msg.peerId, "✅ Длительность полива: 3 минуты", WateringMenu);
      current_keyboard = "WateringMenu";
    }
  });

  bot->onMessage("5 минут", [](VKMessage& msg) {
    if (current_keyboard == "WaterDurationMenu") {
      water_duration_value = 300000;
      preferences.begin("bot-config", false);
      preferences.putInt("water_duration", water_duration_value);
      preferences.end();
      bot->sendWithKeyboard(msg.peerId, "✅ Длительность полива: 5 минут", WateringMenu);
      current_keyboard = "WateringMenu";
    }
  });

  bot->onMessage("10 минут", [](VKMessage& msg) {
    if (current_keyboard == "WaterDurationMenu") {
      water_duration_value = 600000;
      preferences.begin("bot-config", false);
      preferences.putInt("water_duration", water_duration_value);
      preferences.end();
      bot->sendWithKeyboard(msg.peerId, "✅ Длительность полива: 10 минут", WateringMenu);
      current_keyboard = "WateringMenu";
    }
  });

  bot->onMessage("2 часа", [](VKMessage& msg) {
    if (current_keyboard == "LightDurationMenu") {
      light_duration_value = 7200;
      preferences.begin("bot-config", false);
      preferences.putInt("light_duration", light_duration_value);
      preferences.end();
      bot->sendWithKeyboard(msg.peerId, "✅ Длительность света: 2 часа", LightingMenu);
      current_keyboard = "LightingMenu";
    }
  });

  bot->onMessage("4 часа", [](VKMessage& msg) {
    if (current_keyboard == "LightDurationMenu") {
      light_duration_value = 14400;
      preferences.begin("bot-config", false);
      preferences.putInt("light_duration", light_duration_value);
      preferences.end();
      bot->sendWithKeyboard(msg.peerId, "✅ Длительность света: 4 часа", LightingMenu);
      current_keyboard = "LightingMenu";
    }
  });

  bot->onMessage("6 часов", [](VKMessage& msg) {
    if (current_keyboard == "LightDurationMenu") {
      light_duration_value = 21600;
      preferences.begin("bot-config", false);
      preferences.putInt("light_duration", light_duration_value);
      preferences.end();
      bot->sendWithKeyboard(msg.peerId, "✅ Длительность света: 6 часов", LightingMenu);
      current_keyboard = "LightingMenu";
    }
  });

  bot->onMessage("8 часов", [](VKMessage& msg) {
    if (current_keyboard == "LightDurationMenu") {
      light_duration_value = 28800;
      preferences.begin("bot-config", false);
      preferences.putInt("light_duration", light_duration_value);
      preferences.end();
      bot->sendWithKeyboard(msg.peerId, "✅ Длительность света: 8 часов", LightingMenu);
      current_keyboard = "LightingMenu";
    }
  });

  bot->onMessage("12 часов", [](VKMessage& msg) {
    if (current_keyboard == "LightDurationMenu") {
      light_duration_value = 43200;
      preferences.begin("bot-config", false);
      preferences.putInt("light_duration", light_duration_value);
      preferences.end();
      bot->sendWithKeyboard(msg.peerId, "✅ Длительность света: 12 часов", LightingMenu);
      current_keyboard = "LightingMenu";
    }
  });

  bot->onMessage("СИСТЕМНЫЕ НАСТРОЙКИ🔃", [](VKMessage& msg) {
    bot->send(msg.peerId, "🔄 Вход в режим конфигурации...\n\nДанные сохранены!\nСеть ArtWAT_system запускается.\n\nПодключитесь к данной сети WiFi чтобы открыть системные настройки");
    delay(1000);

    preferences.begin("bot-config", false);
    preferences.putBool("goto_config", true);
    preferences.end();

    ESP.restart(); 
  });

  bot->onMessage("НАЗАД↩️", [](VKMessage& msg) {
    if (current_keyboard == "TimeEditMenu") {
      waitingForWaterTime = false;
      waitingUserId = 0;
      activeWaterSlot = -1;
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

  // ===== УНИВЕРСАЛЬНЫЙ ОБРАБОТЧИК =====
  bot->onAnyMessage([](VKMessage& msg) {
    // Сброс состояния, если пользователь ушёл из меню ввода другим путём
    if (activeWaterSlot >= 0 && current_keyboard != "TimeEditMenu") {
      activeWaterSlot = -1;
      waitingForWaterTime = false;
      waitingUserId = 0;
    }

    // --- 1. Режим ввода времени полива (слот) ---
    if (waitingForWaterTime && msg.fromId == waitingUserId && activeWaterSlot >= 0) {
      String t = msg.text;

      if (t.length() == 5 && t[2] == ':') {
        int hh = t.substring(0, 2).toInt();
        int mm = t.substring(3, 5).toInt();
        if (hh >= 0 && hh < 24 && mm >= 0 && mm < 60) {
          bool duplicate = false;
          for (int i = 0; i < WATER_SLOTS; i++) {
            if (i != activeWaterSlot && waterSlots[i] == t) {
              duplicate = true;
              break;
            }
          }
          if (duplicate) {
            bot->send(msg.peerId, "⚠️ Это время уже стоит в другом слоте");
            return;
          }

          int slot = activeWaterSlot;
          waterSlots[slot] = t;
          saveWaterSlots();

          waitingForWaterTime = false;
          waitingUserId = 0;
          activeWaterSlot = -1;

          current_keyboard = "WateringTimeMenu";
          bot->sendWithKeyboard(msg.peerId,
            "✅ Слот сохранён!\n\n" + buildWaterSlotsText(),
            WateringTimeMenu);
          return;
        }
      }

      bot->send(msg.peerId,
        "⚠️ Неверный формат времени.\n\n"
        "Введите время в формате ЧЧ:ММ\n"
        "Например: 09:00, 14:20, 17:15");
      return;
    }

    // --- 2. Режим ввода времени света (один слот) ---
    if (waitingForLightTime && msg.fromId == waitingLightUserId) {
      String t = msg.text;

      if (t.length() == 5 && t[2] == ':') {
        int hh = t.substring(0, 2).toInt();
        int mm = t.substring(3, 5).toInt();
        if (hh >= 0 && hh < 24 && mm >= 0 && mm < 60) {
          waitingForLightTime = false;
          waitingLightUserId = 0;
          lightTime = t;

          preferences.begin("bot-config", false);
          preferences.putString("light_time", lightTime);
          preferences.end();

          current_keyboard = "LightingMenu";
          bot->sendWithKeyboard(msg.peerId,
            "✅ Время включения света установлено: " + lightTime, LightingMenu);
          return;
        }
      }

      bot->send(msg.peerId,
        "⚠️ Неверный формат времени.\n\n"
        "Введите время в формате ЧЧ:ММ\n"
        "Например: 09:00, 14:20, 17:45");
      return;
    }

    // --- 3. Обычная опечатка / неизвестная команда ---
    bot->send(msg.peerId,
      "🤔 Неизвестная команда.\n"
      "Используйте кнопки меню или напишите СТАТУС📊");
  });
}

// ===== ПОДКЛЮЧЕНИЕ К WI-FI =====

void tryConnectWiFi() {
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
  delay(500);

  WiFi.mode(WIFI_STA);
  delay(100);

  WiFi.begin(ssid.c_str(), password.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  Serial.println();
  Serial.print("Статус: "); Serial.println(WiFi.status());

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ Wi-Fi подключен!");
    Serial.print("🌐 IP: ");
    Serial.println(WiFi.localIP());
    staConnected = true;
    systemError = false;

    if (bot == nullptr) {
      preferences.begin("bot-config", true);
      String token = preferences.getString("token", "");
      String groupId = preferences.getString("group_id", "");
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
      triggerSuccessBlink();

      preferences.begin("bot-config", true);
      long lastPeer = preferences.getLong("last_peer", 0);
      preferences.end();

      if (lastPeer != 0) {
        Serial.print("📤 Отправляю приветствие в peerId: ");
        Serial.println(lastPeer);
        bot->sendWithKeyboard(lastPeer,
          "🌱 Система автополива ArtWAT запущена!\n\n"
          "📅 Системное время:\n" + getCurrentDateTime() + "\n\n"
          "Используйте кнопки для управления.",
          MainMenu
        );
      } else {
        Serial.println("ℹ️ Приветствие не отправлено — пользователь ещё не писал боту");
        Serial.println("Напиши боту 'СТАТУС📊' в VK");
      }
    }
  } else {
    Serial.println("❌ Не удалось подключиться.");
    staConnected = false;
    systemError = true;
  }

  lastWifiAttempt = millis();
}

// ===== SERIAL УПРАВЛЕНИЕ =====

void serialPortReading() {
  if (Serial.available() > 0) {

    String incomingString = Serial.readStringUntil('\n');
    incomingString.trim();

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
      Serial.print("Времена полива: ");
      for (int i = 0; i < WATER_SLOTS; i++) {
        Serial.print(waterSlots[i].length() > 0 ? waterSlots[i] : "—");
        if (i < WATER_SLOTS - 1) Serial.print(", ");
      }
      Serial.println();
      Serial.print("Длительность полива: "); Serial.println(String(water_duration_value / 60000));
      Serial.println();
      Serial.print("Время света: "); Serial.println(lightTime);
      Serial.print("Длительность света: "); Serial.println(String(light_duration_value / 3600));
      Serial.println();
      Serial.print("Режим системы: ");
      if (apMode) Serial.println("AP Mode (точка доступа)");
      else if (staConnected) Serial.println("STA Mode (Wi-Fi есть)");
      else Serial.println("Автономный режим");
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
        startPump(true);
        stopLight(true);
        delay(500);
        stopPump(true);
        startLight(true);
        delay(500);
      }
      stopPump(true);
      stopLight(true);
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
    else if (incomingString == "/pump_on") {
      startPump();
      Serial.println("Насос ВКЛЮЧЕН");
      Serial.println("-----");
    }
    else if (incomingString == "/pump_off") {
      stopPump();
      Serial.println("Насос ВЫКЛЮЧЕН");
      Serial.println("-----");
    }
    else if (incomingString == "/light_on") {
      startLight();
      Serial.println("Свет ВКЛЮЧЕН");
      Serial.println("-----");
    }
    else if (incomingString == "/light_off") {
      stopLight();
      Serial.println("Свет ВЫКЛЮЧЕН");
      Serial.println("-----");
    }
    else {
      Serial.print("❌ Неизвестная команда: ");
      Serial.println(incomingString);
      Serial.println("Введите /help для списка команд");
      Serial.println("-----");
    }
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
  delay(500);

  Serial.println();
  Serial.println("🔄 Загрузка системы ArtWAT...");

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

  digitalWrite(STATUS_LED_PIN, HIGH);
  ledState = true;

  initRTC();
  loadSettings();

  // ===== АППАРАТНЫЙ WATCHDOG =====
  esp_task_wdt_deinit();

  esp_task_wdt_config_t wdt_config = {
      .timeout_ms = 90000,
      .idle_core_mask = 0,
      .trigger_panic = true
  };

  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);

  // ===== ПРОВЕРКА WI-FI =====
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
      Serial.println("📶 Wi-Fi настроен — пытаюсь подключиться");
      tryConnectWiFi();

      if (WiFi.status() != WL_CONNECTED) {
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

  if (apMode) {
    dnsServer.processNextRequest();
    server.handleClient();
  }

  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 10000) {
    checkSchedule();
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

      if (bot == nullptr) {
        tryConnectWiFi();
      } else {
        bot->begin();
      }

      Serial.println("✅ STA Mode — бот активирован");
    }

    if (bot != nullptr) {
      bot->loop();
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

    if (millis() - lastWifiAttempt > 300000) {
      Serial.print("🔄 Попытка переподключения");
      tryConnectWiFi();
    }
  }

  static unsigned long portOpenedTime = 0;
  if (!greeted) {
    if (portOpenedTime == 0) {
      portOpenedTime = millis();
    }
    if (millis() - portOpenedTime >= 200) {
      GreetingSerial();
      greeted = true;
    }
  }

  yield();
}
