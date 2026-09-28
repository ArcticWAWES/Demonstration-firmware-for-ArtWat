// -------------------------------  СИСТЕМА АВТОПОЛИВА  -------------------------------
//                             ДЛЯ АВТОНОМНОГО ИСПОЛЬЗОВАНИЯ
//                              УПРАВЛЯЕМАЯ VK СООБЩЕСТВОМ
//                             НА БАЗЕ КАСТОМНОЙ БИБЛИОТЕКИ
//                                    "VKassistent"
//                                   by Arctic_WAWES
// 
//  Прошивка для серии моделей ArtWat (home edition) 
//  Firmwork version: 1.2 DEMO
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
//  
// Распиновка модулей: 
// RTC DS1302:
//    VCC  →  3.3V
//    GND  →  GND
//    CLK  →  GPIO 18
//    DAT  →  GPIO 19
//    RST  →  GPIO 23
//
//  Реле 1 (Насос):
//    VCC  →  5V
//    GND  →  GND
//    IN   →  GPIO 4
//
//  Реле 2 (Лампа):
//    VCC  →  5V
//    GND  →  GND
//    IN   →  GPIO 5
//

#include <VKassistent.h>
#include <ThreeWire.h>
#include <RtcDS1302.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_task_wdt.h>

const char* AP_SSID = "ArtWAT_system";
const char* AP_PASSWORD = "3468#2550";
const String SERIAL_PASSWORD = "3468#2550";

const int IO_PIN = 19;
const int SCLK_PIN = 18;
const int CE_PIN = 23;

ThreeWire myWire(IO_PIN, SCLK_PIN, CE_PIN);
RtcDS1302<ThreeWire> Rtc(myWire);

const int PUMP_PIN = 4;
const int LIGHT_PIN = 5;

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

// ===== ПАРАМЕТРЫ СИСТЕМЫ =====
String current_keyboard = "MainMenu";
String waterTime = "09:00";
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

// ===== FORWARD DECLARATIONS =====
void initBotAndMenus();
void startAPMode();
void stopAPMode();
void tryConnectWiFi();
void serialPortReading();
void GreetingSerial();
void deleteSystemData();

// ===== ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ =====

String getCurrentTime() {
  if (!Rtc.IsDateTimeValid()) return "00:00";
  RtcDateTime now = Rtc.GetDateTime();
  char buf[16];
  sprintf(buf, "%02d:%02d", now.Hour(), now.Minute());
  return String(buf);
}

String getCurrentDateTime() {
  if (!Rtc.IsDateTimeValid()) return "Ошибка RTC";
  RtcDateTime now = Rtc.GetDateTime();
  char buf[32];
  sprintf(buf, "%02d.%02d.%04d %02d:%02d:%02d",
    now.Day(), now.Month(), now.Year(),
    now.Hour(), now.Minute(), now.Second());
  return String(buf);
}

void initRTC() {
  Rtc.Begin();
  Rtc.SetIsWriteProtected(false);
  if (!Rtc.GetIsRunning()) Rtc.SetIsRunning(true);
  
  if (!Rtc.IsDateTimeValid()) {
    RtcDateTime compiled = RtcDateTime(__DATE__, __TIME__);
    Rtc.SetDateTime(compiled);
  }
}

bool setRtcTime(int year, int month, int day, int hour, int minute) {
  if (year < 2000 || year > 2099) return false;
  if (month < 1 || month > 12) return false;
  if (day < 1 || day > 31) return false;
  if (hour < 0 || hour > 23) return false;
  if (minute < 0 || minute > 59) return false;
  
  RtcDateTime newTime(year, month, day, hour, minute, 0);
  Rtc.SetDateTime(newTime);
  return true;
}

void startLight() {
  digitalWrite(LIGHT_PIN, HIGH);
  lightState = true;
  lightStartTime = millis();
}

void stopLight() {
  digitalWrite(LIGHT_PIN, LOW);
  lightState = false;
  lightStartTime = 0;
}

void startPump() {
  digitalWrite(PUMP_PIN, HIGH);
  pumpState = true;
  pumpStartTime = millis();
}

void stopPump() {
  digitalWrite(PUMP_PIN, LOW);
  pumpState = false;
  pumpStartTime = 0;
}

void checkSchedule() {
  if (!Rtc.IsDateTimeValid()) return;
  String currentTime = getCurrentTime();
  
  if (light_duration_value > 0) {
    if (currentTime == lightTime && !lightState) startLight();
    if (lightState && lightStartTime > 0) {
      unsigned long elapsed = millis() - lightStartTime;
      if (elapsed >= (unsigned long)light_duration_value * 1000) stopLight();
    }
  }
  
  if (water_duration_value > 0) {
    if (currentTime == waterTime && !pumpState) startPump();
    if (pumpState && pumpStartTime > 0) {
      unsigned long elapsed = millis() - pumpStartTime;
      if (elapsed >= (unsigned long)water_duration_value) stopPump();
    }
  }
}

String getSystemStatus() {
  String status = "📊 СТАТУС СИСТЕМЫ\n\n";
  status += "💧 ПОЛИВ:\n";
  status += "   • Время: " + waterTime + "\n";
  status += "   • Длительность: " + String(water_duration_value / 60000) + " мин\n";
  status += "   • Статус: " + String(pumpState ? "РАБОТАЕТ" : "ОСТАНОВЛЕН") + "\n\n";
  status += "💡 СВЕТ:\n";
  status += "   • Время включения: " + lightTime + "\n";
  status += "   • Длительность: " + String(light_duration_value / 3600) + " ч\n";
  status += "   • Статус: " + String(lightState ? "ВКЛЮЧЕН" : "ВЫКЛЮЧЕН") + "\n\n";
  status += "📅 Время системы: " + getCurrentDateTime();
  return status;
}

// ===== HTML СТРАНИЦА =====

void handleRoot() {
  preferences.begin("bot-config", true);
  String s_ssid = preferences.getString("ssid", "");
  String s_pass = preferences.getString("password", "");
  String s_token = preferences.getString("token", "");
  String s_group = preferences.getString("group_id", "");
  int savedAdminCount = preferences.getInt("admin_count", 0);
  
  String adminInputs = "";
  if (savedAdminCount == 0) {
    adminInputs = "<div><input type=\"number\" name=\"admin_0\" placeholder=\"ID админа\" style=\"width: 70%; padding: 8px; box-sizing: border-box; margin-bottom: 5px;\"></div>";
  } else {
    for (int i = 0; i < savedAdminCount; i++) {
      String aVal = preferences.getString(("admin_" + String(i)).c_str(), "");
      adminInputs += "<div style=\"margin-bottom: 5px;\"><input type=\"number\" name=\"admin_" + String(i) + "\" value=\"" + aVal + "\" style=\"width: 70%; padding: 8px; box-sizing: border-box;\">";
      if (i > 0) {
        adminInputs += " <button type=\"button\" onclick=\"this.parentElement.remove()\" style=\"padding: 8px;\">✕</button>";
      }
      adminInputs += "</div>";
    }
  }
  preferences.end();

  String html = "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>VKassistent</title></head>";
  html += "<body style=\"font-family: Arial; max-width: 400px; margin: 20px auto; background: #f0f2f5; padding: 20px;\">";
  html += "<h2 style=\"text-align: center;\">Системные настройки ArtWAT</h2>";
  html += "<form action=\"/save\" method=\"POST\" style=\"background: white; padding: 20px; border-radius: 10px;\">";
  
  html += "<label><b>Wi-Fi имя</b></label>";
  html += "<input type=\"text\" name=\"ssid\" value=\"" + s_ssid + "\" required style=\"width: 100%; padding: 8px; margin: 6px 0 10px; box-sizing: border-box;\">";
  
  html += "<label><b>Пароль Wi-Fi</b></label>";
  html += "<input type=\"password\" name=\"password\" value=\"" + s_pass + "\" style=\"width: 100%; padding: 8px; margin: 6px 0 10px; box-sizing: border-box;\">";
  
  html += "<label><b>Токен VK</b></label>";
  html += "<input type=\"text\" name=\"token\" value=\"" + s_token + "\" required style=\"width: 100%; padding: 8px; margin: 6px 0 10px; box-sizing: border-box;\">";
  
  html += "<label><b>ID группы</b></label>";
  html += "<input type=\"text\" name=\"group_id\" value=\"" + s_group + "\" required style=\"width: 100%; padding: 8px; margin: 6px 0 10px; box-sizing: border-box;\">";
  
  html += "<label><b>Админы</b></label>";
  html += "<div id=\"admins\">" + adminInputs + "</div>";
  html += "<button type=\"button\" onclick=\"addAdmin()\" style=\"padding: 6px 12px; margin: 6px 0;\">➕ Добавить</button><hr>";
  
  html += "<label><b>Установить время</b></label>";
  html += "<div style=\"display: flex; gap: 6px; flex-wrap: wrap; margin-bottom: 10px;\">";
  html += "<input type=\"number\" name=\"day\" placeholder=\"День\" min=\"1\" max=\"31\" style=\"width: 28%; padding: 8px; box-sizing: border-box;\">";
  html += "<input type=\"number\" name=\"month\" placeholder=\"Мес\" min=\"1\" max=\"12\" style=\"width: 28%; padding: 8px; box-sizing: border-box;\">";
  html += "<input type=\"number\" name=\"year\" placeholder=\"Год\" min=\"2000\" max=\"2099\" style=\"width: 36%; padding: 8px; box-sizing: border-box;\">";
  html += "<input type=\"number\" name=\"hour\" placeholder=\"Час\" min=\"0\" max=\"23\" style=\"width: 47%; padding: 8px; box-sizing: border-box;\">";
  html += "<input type=\"number\" name=\"minute\" placeholder=\"Мин\" min=\"0\" max=\"59\" style=\"width: 47%; padding: 8px; box-sizing: border-box;\">";
  html += "</div>";
  
  html += "<input type=\"submit\" value=\"Сохранить\" style=\"width: 100%; padding: 10px; background: #4c75a3; color: white; border: none; font-weight: bold; border-radius: 6px; cursor: pointer;\">";
  html += "</form>";
  
  html += "<script>let adminCount = " + String(savedAdminCount > 0 ? savedAdminCount : 1) + ";";
  html += "function addAdmin() { const container = document.getElementById('admins'); const div = document.createElement('div'); div.style.marginBottom = '5px';";
  html += "div.innerHTML = `<input type=\"number\" name=\"admin_${adminCount}\" placeholder=\"ID админа\" style=\"width: 70%; padding: 8px; box-sizing: border-box;\"> <button type=\"button\" onclick=\"this.parentElement.remove()\" style=\"padding: 8px;\">✕</button>`;";
  html += "container.appendChild(div); adminCount++; }</script></body></html>";
  
  server.send(200, "text/html", html);
}

void loadSettings() {
  preferences.begin("bot-config", true);
  String stored_ssid = preferences.getString("ssid", "");
  bool goto_config = preferences.getBool("goto_config", false);
  
  if (stored_ssid.length() == 0 || goto_config) {
    configMode = true;
  }
  
  waterTime = preferences.getString("water_time", "09:00");
  lightTime = preferences.getString("light_time", "18:00");
  water_duration_value = preferences.getInt("water_duration", 0);
  light_duration_value = preferences.getInt("light_duration", 0);
  
  preferences.end();
}

void handleSave() {
  if (server.hasArg("ssid") && server.hasArg("token") && server.hasArg("group_id")) {
    preferences.begin("bot-config", false);
    
    preferences.putString("ssid", server.arg("ssid"));
    preferences.putString("password", server.arg("password"));
    preferences.putString("token", server.arg("token"));
    preferences.putString("group_id", server.arg("group_id"));
    preferences.putBool("goto_config", false);
    
    preferences.putString("water_time", waterTime);
    preferences.putString("light_time", lightTime);
    preferences.putInt("water_duration", water_duration_value);
    preferences.putInt("light_duration", light_duration_value);
    
    int adminCount = 0;
    for (int i = 0; i < 10; i++) {
      String key = "admin_" + String(i);
      if (server.hasArg(key) && server.arg(key) != "") {
        String pKey = "admin_" + String(adminCount);
        preferences.putString(pKey.c_str(), server.arg(key));
        adminCount++;
      }
    }
    preferences.putInt("admin_count", adminCount);
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

// ===== ИНИЦИАЛИЗАЦИЯ VK-БОТА И МЕНЮ =====

void initBotAndMenus() {
  MainMenu = createKeyboard(
    Button("СТАТУС📊", secondary),
    Button("НАСТРОЙКИ⚙️", secondary),
    Button("СИСТЕМНЫЕ НАСТРОЙКИ🔃", primary)
  );

  SettingsMenu = createKeyboard(
    Button("ПОЛИВ💧", secondary),
    Button("СВЕТ💡", secondary),
    Button("НАЗАД↩️", primary)
  );

  WateringMenu = createKeyboard(
    Button("ДЛИТЕЛЬНОСТЬ💧⏱", secondary),
    Button("ВРЕМЯ ПОЛИВА💧🕐", secondary),
    Button("НАЗАД↩️", primary)
  );

  LightingMenu = createKeyboard(
    Button("ДЛИТЕЛЬНОСТЬ💡⏱", secondary),
    Button("ВРЕМЯ ВКЛЮЧЕНИЯ💡🕐", secondary),
    Button("ВКЛЮЧИТЬ💡", positive),
    Button("ВЫКЛЮЧИТЬ💡", negative),
    Button("НАЗАД↩️", primary)
  );

  WaterDurationMenu = createKeyboard(
    Button("1 минуту", secondary),
    Button("2 минуты", secondary),
    Button("3 минуты", secondary),
    Button("5 минут", secondary),
    Button("10 минут", secondary),
    Button("НАЗАД↩️", primary)
  );

  LightDurationMenu = createKeyboard(
    Button("2 часа", secondary),
    Button("4 часа", secondary),
    Button("6 часов", secondary),
    Button("8 часов", secondary),
    Button("12 часов", secondary),
    Button("НАЗАД↩️", primary)
  );

  for (int i = 0; i < 24; i++) {
    char buf[16];
    sprintf(buf, "%02d:00", i);
    String timeStr = String(buf);
    
    bot->proccesMessage(timeStr, [timeStr](long UserID, String text) {
      if (waitingForWaterTime && UserID == waitingUserId) {
        waitingForWaterTime = false;
        waterTime = timeStr;
        preferences.begin("bot-config", false);
        preferences.putString("water_time", waterTime);
        preferences.end();
        current_keyboard = "WateringMenu";
        bot->sendWithKeyboard(UserID, "✅ Время полива установлено: " + waterTime, WateringMenu);
        return;
      }
      if (waitingForLightTime && UserID == waitingLightUserId) {
        waitingForLightTime = false;
        lightTime = timeStr;
        preferences.begin("bot-config", false);
        preferences.putString("light_time", lightTime);
        preferences.end();
        current_keyboard = "LightingMenu";
        bot->sendWithKeyboard(UserID, "✅ Время включения света установлено: " + lightTime, LightingMenu);
        return;
      }
    });
  }

  bot->proccesMessage("НАСТРОЙКИ⚙️", [](long UserID, String text) {
    current_keyboard = "SettingsMenu";
    bot->sendWithKeyboard(UserID, "НАСТРОЙКИ⚙️\n\nВыберите раздел для редактирования параметров.", SettingsMenu);
  });

  bot->proccesMessage("СТАТУС📊", [](long UserID, String text) {
    bot->send(UserID, getSystemStatus());
  });

  bot->proccesMessage("ПОЛИВ💧", [](long UserID, String text) {
    if (current_keyboard == "SettingsMenu") {
      current_keyboard = "WateringMenu";
      bot->sendWithKeyboard(UserID, "ПОЛИВ💧\n\nРаздел для редактирования параметров полива", WateringMenu);
    }
  });

  bot->proccesMessage("ДЛИТЕЛЬНОСТЬ💧⏱", [](long UserID, String text) {
    if (current_keyboard == "WateringMenu") {
      current_keyboard = "WaterDurationMenu";
      bot->sendWithKeyboard(UserID, "⏱ ВЫБЕРИТЕ ДЛИТЕЛЬНОСТЬ ПОЛИВА", WaterDurationMenu);
    }
  });

  bot->proccesMessage("ВРЕМЯ ПОЛИВА💧🕐", [](long UserID, String text) {
    if (current_keyboard == "WateringMenu") {
      waitingForWaterTime = true;
      waitingUserId = UserID;
      
      String message = "⏰ ВВЕДИТЕ ВРЕМЯ ПОЛИВА\n\n";
      message += "Напишите время в формате ЧЧ:00\n";
      message += "Например: 14:00, 09:00, 18:00\n\n";
      message += "Доступные часы:\n";
      message += "00:00  01:00  02:00  03:00\n";
      message += "04:00  05:00  06:00  07:00\n";
      message += "08:00  09:00  10:00  11:00\n";
      message += "12:00  13:00  14:00  15:00\n";
      message += "16:00  17:00  18:00  19:00\n";
      message += "20:00  21:00  22:00  23:00\n\n";
      message += "Текущее время полива: " + waterTime;
      
      bot->send(UserID, message);
    }
  });

  bot->proccesMessage("СВЕТ💡", [](long UserID, String text) {
    if (current_keyboard == "SettingsMenu") {
      current_keyboard = "LightingMenu";
      bot->sendWithKeyboard(UserID, "СВЕТ💡\n\nРаздел редактирования параметров лампы.", LightingMenu);
    }
  });

  bot->proccesMessage("ДЛИТЕЛЬНОСТЬ💡⏱", [](long UserID, String text) {
    if (current_keyboard == "LightingMenu") {
      current_keyboard = "LightDurationMenu";
      bot->sendWithKeyboard(UserID, "⏱ ВЫБЕРИТЕ ДЛИТЕЛЬНОСТЬ РАБОТЫ ЛАМПЫ", LightDurationMenu);
    }
  });

  bot->proccesMessage("ВРЕМЯ ВКЛЮЧЕНИЯ💡🕐", [](long UserID, String text) {
    if (current_keyboard == "LightingMenu") {
      waitingForLightTime = true;
      waitingLightUserId = UserID;
      
      String message = "⏰ ВВЕДИТЕ ВРЕМЯ ВКЛЮЧЕНИЯ СВЕТА\n\n";
      message += "Напишите время в формате ЧЧ:00\n";
      message += "Например: 14:00, 09:00, 18:00\n\n";
      message += "Доступные часы:\n";
      message += "00:00  01:00  02:00  03:00\n";
      message += "04:00  05:00  06:00  07:00\n";
      message += "08:00  09:00  10:00  11:00\n";
      message += "12:00  13:00  14:00  15:00\n";
      message += "16:00  17:00  18:00  19:00\n";
      message += "20:00  21:00  22:00  23:00\n\n";
      message += "Текущее время включения: " + lightTime;
      
      bot->send(UserID, message);
    }
  });

  bot->proccesMessage("ВКЛЮЧИТЬ💡", [](long UserID, String text) {
    if (current_keyboard == "LightingMenu") {
      if (!lightState) {
        startLight();
        bot->send(UserID, "💡 Свет включён вручную");
      } else {
        bot->send(UserID, "💡 Свет уже включён");
      }
    }
  });

  bot->proccesMessage("ВЫКЛЮЧИТЬ💡", [](long UserID, String text) {
    if (current_keyboard == "LightingMenu") {
      if (lightState) {
        stopLight();
        bot->send(UserID, "💡 Свет выключен вручную");
      } else {
        bot->send(UserID, "💡 Свет уже выключен");
      }
    }
  });

  bot->proccesMessage("1 минуту", [](long UserID, String text) {
    if (current_keyboard == "WaterDurationMenu") {
      water_duration_value = 60000;
      preferences.begin("bot-config", false);
      preferences.putInt("water_duration", water_duration_value);
      preferences.end();
      bot->sendWithKeyboard(UserID, "✅ Длительность полива: 1 минута", WateringMenu);
      current_keyboard = "WateringMenu";
    }
  });

  bot->proccesMessage("2 минуты", [](long UserID, String text) {
    if (current_keyboard == "WaterDurationMenu") {
      water_duration_value = 120000;
      preferences.begin("bot-config", false);
      preferences.putInt("water_duration", water_duration_value);
      preferences.end();
      bot->sendWithKeyboard(UserID, "✅ Длительность полива: 2 минуты", WateringMenu);
      current_keyboard = "WateringMenu";
    }
  });

  bot->proccesMessage("3 минуты", [](long UserID, String text) {
    if (current_keyboard == "WaterDurationMenu") {
      water_duration_value = 180000;
      preferences.begin("bot-config", false);
      preferences.putInt("water_duration", water_duration_value);
      preferences.end();
      bot->sendWithKeyboard(UserID, "✅ Длительность полива: 3 минуты", WateringMenu);
      current_keyboard = "WateringMenu";
    }
  });

  bot->proccesMessage("5 минут", [](long UserID, String text) {
    if (current_keyboard == "WaterDurationMenu") {
      water_duration_value = 300000;
      preferences.begin("bot-config", false);
      preferences.putInt("water_duration", water_duration_value);
      preferences.end();
      bot->sendWithKeyboard(UserID, "✅ Длительность полива: 5 минут", WateringMenu);
      current_keyboard = "WateringMenu";
    }
  });

  bot->proccesMessage("10 минут", [](long UserID, String text) {
    if (current_keyboard == "WaterDurationMenu") {
      water_duration_value = 600000;
      preferences.begin("bot-config", false);
      preferences.putInt("water_duration", water_duration_value);
      preferences.end();
      bot->sendWithKeyboard(UserID, "✅ Длительность полива: 10 минут", WateringMenu);
      current_keyboard = "WateringMenu";
    }
  });

  bot->proccesMessage("2 часа", [](long UserID, String text) {
    if (current_keyboard == "LightDurationMenu") {
      light_duration_value = 7200;
      preferences.begin("bot-config", false);
      preferences.putInt("light_duration", light_duration_value);
      preferences.end();
      bot->sendWithKeyboard(UserID, "✅ Длительность света: 2 часа", LightingMenu);
      current_keyboard = "LightingMenu";
    }
  });

  bot->proccesMessage("4 часа", [](long UserID, String text) {
    if (current_keyboard == "LightDurationMenu") {
      light_duration_value = 14400;
      preferences.begin("bot-config", false);
      preferences.putInt("light_duration", light_duration_value);
      preferences.end();
      bot->sendWithKeyboard(UserID, "✅ Длительность света: 4 часа", LightingMenu);
      current_keyboard = "LightingMenu";
    }
  });

  bot->proccesMessage("6 часов", [](long UserID, String text) {
    if (current_keyboard == "LightDurationMenu") {
      light_duration_value = 21600;
      preferences.begin("bot-config", false);
      preferences.putInt("light_duration", light_duration_value);
      preferences.end();
      bot->sendWithKeyboard(UserID, "✅ Длительность света: 6 часов", LightingMenu);
      current_keyboard = "LightingMenu";
    }
  });

  bot->proccesMessage("8 часов", [](long UserID, String text) {
    if (current_keyboard == "LightDurationMenu") {
      light_duration_value = 28800;
      preferences.begin("bot-config", false);
      preferences.putInt("light_duration", light_duration_value);
      preferences.end();
      bot->sendWithKeyboard(UserID, "✅ Длительность света: 8 часов", LightingMenu);
      current_keyboard = "LightingMenu";
    }
  });

  bot->proccesMessage("12 часов", [](long UserID, String text) {
    if (current_keyboard == "LightDurationMenu") {
      light_duration_value = 43200;
      preferences.begin("bot-config", false);
      preferences.putInt("light_duration", light_duration_value);
      preferences.end();
      bot->sendWithKeyboard(UserID, "✅ Длительность света: 12 часов", LightingMenu);
      current_keyboard = "LightingMenu";
    }
  });

  bot->proccesMessage("СИСТЕМНЫЕ НАСТРОЙКИ🔃", [](long UserID, String text) {
    bot->send(UserID, "🔄 Вход в режим конфигурации...\n\nДанные сохранены!\nСеть ArtWAT_system запускается.\n\nПодключитесь к данной сети WiFi чтобы открыть системные настройки");
    delay(1000);
    
    preferences.begin("bot-config", false);
    preferences.putBool("goto_config", true);
    preferences.end();
    
    ESP.restart(); 
  });

  bot->proccesMessage("НАЗАД↩️", [](long UserID, String text) {
    if (current_keyboard == "WaterDurationMenu") {
      current_keyboard = "WateringMenu";
      bot->sendWithKeyboard(UserID, "ПОЛИВ💧 НАСТРОЙКИ", WateringMenu);
    } else if (current_keyboard == "LightDurationMenu") {
      current_keyboard = "LightingMenu";
      bot->sendWithKeyboard(UserID, "СВЕТ💡 НАСТРОЙКИ", LightingMenu);
    } else if (current_keyboard == "WateringMenu") {
      current_keyboard = "SettingsMenu";
      bot->sendWithKeyboard(UserID, "НАСТРОЙКИ⚙️", SettingsMenu);
    } else if (current_keyboard == "LightingMenu") {
      current_keyboard = "SettingsMenu";
      bot->sendWithKeyboard(UserID, "НАСТРОЙКИ⚙️", SettingsMenu);
    } else if (current_keyboard == "SettingsMenu") {
      current_keyboard = "MainMenu";
      bot->sendWithKeyboard(UserID, "ГЛАВНОЕ МЕНЮ🌱", MainMenu);
    } else {
      current_keyboard = "MainMenu";
      bot->sendWithKeyboard(UserID, "ГЛАВНОЕ МЕНЮ🌱", MainMenu);
    }
  });
}

// ===== ПОДКЛЮЧЕНИЕ К WI-FI =====

void tryConnectWiFi() {
  Serial.println("📶 Попытка подключения к Wi-Fi...");
  
  preferences.begin("bot-config", true);
  String ssid = preferences.getString("ssid", "");
  String password = preferences.getString("password", "");
  preferences.end();
  
  if (ssid.length() == 0) {
    Serial.println("⚠️ Wi-Fi не настроен.");
    lastWifiAttempt = millis();
    return;
  }
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ Wi-Fi подключен!");
    Serial.print("🌐 IP: ");
    Serial.println(WiFi.localIP());
    staConnected = true;
    
    if (bot == nullptr) {
      preferences.begin("bot-config", true);
      String token = preferences.getString("token", "");
      String groupId = preferences.getString("group_id", "");
      int adminCount = preferences.getInt("admin_count", 0);
      preferences.end();
      
      if (token.length() == 0 || groupId.length() == 0) {
        Serial.println("❌ Токен или Group ID пустые!");
        Serial.println("Откройте веб-интерфейс и введите данные.");
        return;
      }
      
      bot = new VKassistent(token, groupId);
      bot->connectWIFI(ssid, password);
      bot->begin();
      
      preferences.begin("bot-config", true);
      for (int i = 0; i < adminCount; i++) {
        String pKey = "admin_" + String(i);
        String adminStr = preferences.getString(pKey.c_str(), "0");
        long adminID = strtol(adminStr.c_str(), NULL, 10);
        bot->addAdmin(adminID);
      }
      preferences.end();
      
      initBotAndMenus();
      
      preferences.begin("bot-config", true);
      String firstAdminStr = preferences.getString("admin_0", "0");
      preferences.end();
      long firstAdminID = strtol(firstAdminStr.c_str(), NULL, 10);
      
      if (firstAdminID > 0) {
        bot->sendWithKeyboard(firstAdminID, 
          "🌱 Система автополива ArtWAT запущена!\n\n📅 Системное время:\n " + getCurrentDateTime() + "\n\nИспользуйте кнопки для управления.", 
          MainMenu
        );
      }
    }
  } else {
    Serial.println("\n❌ Не удалось подключиться.");
    staConnected = false;
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
      Serial.print("Время полива: "); Serial.println(waterTime);
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
      preferences.putString("water_time", waterTime);
      preferences.putString("light_time", lightTime);
      preferences.putInt("water_duration", water_duration_value);
      preferences.putInt("light_duration", light_duration_value);
      preferences.end();
      
      Serial.println("✅ Параметры сохранены в память!");
      Serial.println("-----");
    }
    else if (incomingString == "/relay_test") {
      Serial.println("Выполняю проверку выходов...");
      for (int i = 0; i < 10; i++) {
        digitalWrite(4, HIGH);
        digitalWrite(5, LOW);
        delay(500);
        digitalWrite(5, HIGH);
        digitalWrite(4, LOW);
        delay(500);
      }
      digitalWrite(4, LOW);
      digitalWrite(5, LOW);
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
  
  pinMode(PUMP_PIN, OUTPUT);
  pinMode(LIGHT_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, HIGH);
  digitalWrite(LIGHT_PIN, HIGH);
  
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
  } else {
    preferences.begin("bot-config", true);
    String stored_ssid = preferences.getString("ssid", "");
    preferences.end();
    
    if (stored_ssid.length() == 0) {
      Serial.println("📡 Wi-Fi не настроен — AP Mode");
      startAPMode();
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
}

// ===== LOOP =====

void loop() {
  esp_task_wdt_reset();
  
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
    if (apMode) {
      stopAPMode();
      Serial.println("✅ Wi-Fi есть — AP Mode отключён");
    }
    
    if (!staConnected) {
      staConnected = true;
      wifiLostTime = 0;
      
      if (bot == nullptr) {
        initBotAndMenus();
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
