// -------------------------------  СИСТЕМА АВТОПОЛИВА  -------------------------------
//                             ДЛЯ ПОЛУАВТОНОМНОГО ИСПОЛЬЗОВАНИЯ
//                              УПРАВЛЯЕМАЯ VK СООБЩЕСТВОМ
//                             НА БАЗЕ КАСТОМНОЙ БИБЛИОТЕКИ
//                                    "VKassistent"
//                                   by Arctic_WAWES
// 
//  Прошивка для серии моделей ArtWat BETA/DEMO (home edition)
//  Firmwork version: 1.1.3 DEMO
// 
//  Автономная система автоматического полива, управляемая с помощью VK сообщества (бота).
//  Взаимодействия происходят с помощью авторской библиотеки "VKassistent" и созданного
//  пользователем сообществом в котором находится непосредственно сам бот.
// 
//  Особенности:
//  - Подключение устройства происходит с помощью локальной HTML страницы, открываемой после
//  подключения к создаваемой микроконтроллером точке доступа, на ней вводятся параметры
//  сети WiFi и пользовательского VK сообщества.
//  - За счёт использования кастомной библиотеки в код не выносятся HTTP и Json 
//  настройки, которые сильно ухудшали визуал и чтение кода.
//  - В случае неполадок в работе сервисов VK или внутренних поломок считывать показания
//  и управлять системой можно с помощью Serial монитора, войдя в режим администратора 
//  отправляя команды через кабель на микроконтроллер напрямую.
//  - Автосохранение всех внесённых парамтеров, начиная от системного времени, заканчивая
//  временем включения насоса, лампы и т.д.

#include <VKassistent.h>
#include <ThreeWire.h>
#include <RtcDS1302.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

const char* AP_SSID = "ArtWAT_system";
const char* AP_PASSWORD = "12345678";        // ← Пароль от точки доступа (режима конфиграции)
const String SERIAL_PASSWORD = "3468#2550";  // ← Пароль от режима разработчика

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

bool configMode = false;

VKassistent* bot = nullptr;

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
bool waitingForDeleteConfirm = false;
bool serialAuthorized = false;
bool greeted = false;

String MainMenu;
String SettingsMenu;
String WateringMenu;
String LightingMenu;
String WaterDurationMenu;
String LightDurationMenu;

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
  
  // ===== ЗАГРУЖАЕМ ПАРАМЕТРЫ ПОЛИВА И СВЕТА =====
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
    
    // ===== СОХРАНЯЕМ ПАРАМЕТРЫ ПОЛИВА И СВЕТА =====
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

void startConfigMode() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, nullptr);  // Без пароля
  
  dnsServer.start(DNS_PORT, "*", apIP);
  
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound([]() {
    server.sendHeader("Location", String("http://") + apIP.toString(), true);
    server.send(302, "text/plain", "");
  });
  
  server.begin();
  Serial.println("📡 Точка доступа успешно поднята: ArtWAT_system");
}

void deleteSystemData() {
  // чистка данных из памяти
  preferences.begin("bot-config", false);
  preferences.clear();
  preferences.end();

  delay(1500);
  Serial.flush();
  ESP.restart();
}

void startNormalMode() {
  preferences.begin("bot-config", true);
  String ssid = preferences.getString("ssid", "");
  String password = preferences.getString("password", "");
  String token = preferences.getString("token", "");
  String groupId = preferences.getString("group_id", "");
  int adminCount = preferences.getInt("admin_count", 0);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n❌ Не удалось подключиться. Возврат в режим точки доступа.");
    preferences.end();
    configMode = true;
    startConfigMode();
    return;
  }
  
  Serial.println("\n✅ Wi-Fi успешно подключен!");
  
  bot = new VKassistent(token, groupId);
  bot->connectWIFI(ssid, password);
  bot->begin();
  
  for (int i = 0; i < adminCount; i++) {
    String pKey = "admin_" + String(i);
    String adminStr = preferences.getString(pKey.c_str(), "0");
    long adminID = strtol(adminStr.c_str(), NULL, 10);
    bot->addAdmin(adminID);
  }
  
  String firstAdminStr = preferences.getString("admin_0", "0");
  long firstAdminID = strtol(firstAdminStr.c_str(), NULL, 10);
  preferences.end();

  // Меню с эмодзи ПОСЛЕ текста
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

  // Обработчики для ввода времени (с автосохранением)
  for (int i = 0; i < 24; i++) {
    char buf[16];
    sprintf(buf, "%02d:00", i);
    String timeStr = String(buf);
    
    bot->proccesMessage(timeStr, [timeStr](long UserID, String text) {
      if (waitingForWaterTime && UserID == waitingUserId) {
        waitingForWaterTime = false;
        waterTime = timeStr;
        
        // ===== АВТОСОХРАНЕНИЕ =====
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
        
        // ===== АВТОСОХРАНЕНИЕ =====
        preferences.begin("bot-config", false);
        preferences.putString("light_time", lightTime);
        preferences.end();
        
        current_keyboard = "LightingMenu";
        bot->sendWithKeyboard(UserID, "✅ Время включения света установлено: " + lightTime, LightingMenu);
        return;
      }
    });
  }

  // Обновленные обработчики с новыми названиями кнопок
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

  // ОБНОВЛЕННЫЙ ОБРАБОТЧИК ДЛЯ ВРЕМЕНИ ПОЛИВА С РАСПИСАНИЕМ
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

  // ОБНОВЛЕННЫЙ ОБРАБОТЧИК ДЛЯ ВРЕМЕНИ ВКЛЮЧЕНИЯ СВЕТА С РАСПИСАНИЕМ
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

  // Обработчики для длительности полива (с автосохранением)
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

  // Обработчики для длительности света (с автосохранением)
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

  if (firstAdminID > 0) {
    bot->sendWithKeyboard(firstAdminID, 
      "🌱 Система автополива ArtWAT запущена!\n\n📅 Системное время:\n " + getCurrentDateTime() + "\n\nИспользуйте кнопки для управления.", 
      MainMenu
    );
  }
}

// -----------------------------
//  Функция чтения Serial порта для отправки и сбора информации с системы
// -----------------------------
void serialPortReading() {
  if (Serial.available() > 0) {

    String incomingString = Serial.readStringUntil('\n');
    incomingString.trim();

    // ===== АВТОРИЗАЦИЯ =====
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

    // ===== ЗАПРОС НА УДАЛЕНИЕ =====
    if (incomingString == "/delete_system_data") {
      waitingForDeleteConfirm = true;

      Serial.println("Вы удаляете все сохранённые системные данные");
      Serial.println("Вы УВЕРЕНЫ в том что сбрасываете систему до заводских настроек?");
      Serial.println("Введите команду /agree чтобы подтвердить действие, или");
      Serial.println("введите любую другую команду чтобы отменить сброс.");
    }

    // ===== ПОДТВЕРЖДЕНИЕ УДАЛЕНИЯ =====
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

    // ===== ПРОВЕРКА ПАРАМЕТРОВ =====
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
      Serial.print("Режим системы: "); Serial.println(configMode ? "Ожидание Сети..." : "Рабочий");
      Serial.println("=========================");
    }

    // ===== СОХРАНЕНИЕ ПАРАМЕТРОВ =====
    else if (incomingString == "/save_params") {
      preferences.begin("bot-config", false);
      preferences.putString("water_time", waterTime);
      preferences.putString("light_time", lightTime);
      preferences.putInt("water_duration", water_duration_value);
      preferences.putInt("light_duration", light_duration_value);
      preferences.end();
      
      Serial.println("✅ Параметры сохранены в память!");
      Serial.println();
      Serial.print("Время полива: "); Serial.println(waterTime);
      Serial.print("Длительность полива: "); Serial.println(String(water_duration_value / 60000) + " мин");
      Serial.println();
      Serial.print("Время света: "); Serial.println(lightTime);
      Serial.print("Длительность света: "); Serial.println(String(light_duration_value / 3600) + " ч");
      Serial.println("-----");
    }

    // ===== ТЕСТ РЕЛЕ =====
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

    // ===== ПЕРЕЗАГРУЗКА =====
    else if (incomingString == "/reboot_system") {
      Serial.println("Перезагрузка системы...");
      Serial.println("После перезагрузки нужно будет повторно авторизироваться.");
      Serial.println("Команда чтобы авторизироваться - /login");
      Serial.println("-----");
      delay(500);
      ESP.restart();
    }

    // ===== РЕЖИМ КОНФИГУРАЦИИ =====
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

    // ===== ВЫХОД ИЗ РЕЖИМА АДМИНИСТРАТОРА =====
    else if (incomingString == "/logout") {
      serialAuthorized = false;

      Serial.println("Вы вышли из режима администратора");
      GreetingSerial();
    }

    // ===== СПРАВКА =====
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

    // ===== УПРАВЛЕНИЕ НАСОСОМ =====
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

    // ===== УПРАВЛЕНИЕ СВЕТОМ =====
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

    // ===== НЕИЗВЕСТНАЯ КОМАНДА =====
    else {
      Serial.print("❌ Неизвестная команда: ");
      Serial.println(incomingString);
      Serial.println("Введите /help для списка команд");
      Serial.println("-----");
    }

  }
}
// -----------------------------

void GreetingSerial() {
  Serial.println("=== ArtWat system ===");
  Serial.println("Войдите в режим разработчика чтобы системно управлять параметрами.");
  Serial.println("Команда чтобы авторизироваться - /login");
  Serial.println("=====================");
}

void setup() {
  Serial.begin(115200);

  pinMode(PUMP_PIN, OUTPUT);
  pinMode(LIGHT_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, HIGH);
  digitalWrite(LIGHT_PIN, HIGH);
  
  initRTC();
  loadSettings();
  
  if (configMode) {
    startConfigMode();
  } else {
    startNormalMode();
  }
}

void loop() {
  serialPortReading();

  if (configMode) {
    dnsServer.processNextRequest();
    server.handleClient();
  } else {
    if (bot != nullptr) {
      bot->loop();
    }
    
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck > 10000) {
      checkSchedule();
      lastCheck = millis();
    }
  }

  // --- Проверка монитора порта без delay() ---
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
