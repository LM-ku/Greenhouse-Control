#define FW_VERSION "Watering_2020-03-19"

/*
  EEPROM MAP :
  addr 0...99     - для проверки состояния eeprom : == ap_ssid, иначе произвольные значения
  addr 110        - память состояния битовых переменных
                    bit 0 : man_cmd
                    bit 1 : rem_cmd
                    bit 2 : auto_cmd
                    bit 3 : watering_state 
                    bit 4 :
                    bit 5 :
                    bit 6 :
                    bit 7 :
  addr 114...117  - mode_ctrl (int)
  addr 120...123  - set_temp (float)
  addr 124...127  - set_hum (float)
  addr 128...131  - set_moist (float)
  addr 132...135  - air_val (int)     - показания датчика влажности почвы, соответствующие 0%
  addr 136...139  - water_val (int)   - показания датчика влажности почвы, соответствующие 100%
*/

// === Library ===
#include <ipaddress.h>
#include <TimeLib.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <PubSubClient.h>
#include <EEPROM.h>
#include <Wire.h>
#include <BME280I2C.h>

// === Index ===
byte n = 1;

// === WiFi AP Mode ===
IPAddress ap_ip(192, 168, 123, (20 + n));
IPAddress ap_gateway(192, 168, 123, (20 + n));
IPAddress ap_subnet(255, 255, 255, 0);
String ap_ssid = "BGH_watering_" + String(n);
String ap_pass = "12345678";

// === WiFi STA Mode ===
IPAddress ip, gw, mask;
String ssid, pass;
boolean sta_ok = false;   // подключение к роутеру состоялось

// === MQTT broker ===
char* mqtt_server;
int mqtt_port;
String topic;
String mqtt_client;
String mqtt_user = "";
String mqtt_pass = "";

byte step_mqtt_send = 0;  // переменная для поочередной (по одному сообщению за цикл loop) публикации сообщений на MQTT брокере
boolean mqtt_send = false;

WiFiClient espClient;
PubSubClient client(espClient);


// === WEB SERVER ===
ESP8266WebServer server(80);
const char* root_str = "<!DOCTYPE HTML><html><head><meta http-equiv='refresh' content='1; /'></head></html>\n\r";
const char* ctrl_str = "<!DOCTYPE HTML><html><head><meta http-equiv='refresh' content='1; /ctrl_mode'></head></html>\n\r";

// === Capacitive Soil Moisture Sensor ===
int sensor_moist;
int air_val = 520;  // показания датчика на воздухе, соответствуют 0%
int water_val = 260;  // показания датчика в воде, соответствуют 100%
float set_moist = 65.0; // оптимальная влажность почвы 60...70% или более
float moist;
boolean moist_sensor;
boolean moist_ctrl = false;

// === GPIO ===
const byte tmr_rst = 16;
const byte tmr_opn = 12;
const byte tmr_cls = 14;
const byte valve = 13;
const byte ESP_BUILTIN_LED = 2;

// === CTRL - управление ===
byte mode_ctrl;             // режим: 0 = auto, 1 = remote, 2 = manual
byte last_watering_day, last_watering_hour, last_watering_minute;
boolean mem_ctrl;
boolean eeprom_ok;          // true == EEPROM содержит актуальные данные
boolean first_watering;     // для определения первого полива после перезагрузки
boolean watering_state;     
boolean man_cmd;
boolean rem_cmd;            // дистанционная команда для реле клапана полива: false = выключить, true = включить
volatile boolean auto_cmd;  // команда для реле клапана полива в автономном режиме: false = выключить, true = включить

// === ТАЙМЕРЫ СОБЫТИЙ ===
long eventTime;             // для снятия текущего времени с начала старта
long event_reconnect;       // для переподключения
long event_sensors_read;    // для опроса датчиков
long event_serial_output;
long event_mqtt_publish;
long event_tmr_rst;         // для формирования импульса сброса таймера 
long start_watering;        // старт полива
long end_watering;          // окончание полива
long after_watering;        // прошло после окончания полива
long watering_cont;         // продолжительность полива
time_t mem;

//-----------------------------------------------------------------------------
//                     ПРОВЕРКА ДОСТУПНОСТИ WiFi СЕТИ 
//-----------------------------------------------------------------------------
boolean scanWIFI(String found) {
  uint8_t n = WiFi.scanNetworks();
  for (uint8_t i = 0; i < n; i++) {
    String ssidMy = WiFi.SSID(i);
    if (ssidMy == found) return true;
  }
  return false;
}

//-----------------------------------------------------------------------------
//                       ОПРОС ДАТЧИКА ВЛАЖНОСТИ ПОЧВЫ
//-----------------------------------------------------------------------------
void sensors_read() {
  event_sensors_read = millis();
  moist_sensor = true;
  int sensor_moist = analogRead(0);
  if (sensor_moist >= air_val) {
    moist = 0.0;
    moist_sensor = false;
  }
  if (sensor_moist <= water_val) {
    moist = 100.0;
    moist_sensor = false;
  }

  if (!moist_sensor) moist_ctrl = true; // при этом датчик влажности никак не влияет на включение клапана по таймеру
  else {
    moist = 100.0 * float(air_val - sensor_moist) / float(air_val - water_val);

    // *** коррекция управления реле клапана полива по датчику влажности
    if (moist >= set_moist + 5.0) moist_ctrl = false;
    if (moist <= set_moist - 5.0) moist_ctrl = true;
  }

  // *** вывод данных от датчика в порт
  Serial.print ("Влажность почвы   ");
  Serial.print (moist);
  Serial.print (" %  [");
  Serial.print (sensor_moist);
  Serial.println ("]");
}

//-----------------------------------------------------------------------------
//                    КОМАНДА УПРАВЛЕНИЯ РЕЛЕ КЛАПАНА ПОЛИВА
//-----------------------------------------------------------------------------
boolean valve_ctrl() {
  boolean out = ((mode_ctrl == 0) && auto_cmd && moist_ctrl) || ((mode_ctrl == 1) && rem_cmd) || ((mode_ctrl == 2) && man_cmd);
  if (out != mem_ctrl) handleRoot(); // обновить главную страницу при изменении состояния выхода управления
  mem_ctrl = out;
  return out;
}

// ===== ПРЕОБРАЗОВАНИE МИНУТ И СЕКУНД ИЗ INT В STRING ВИДА "ХХ" =====
String digits_to_String(int digits) {
  if (digits < 10) return "0" + String(digits);
  return String(digits);
}

// ===== ПРЕОБРАЗОВАНИE RSSI dBm В ПРОЦЕНТЫ =====
unsigned int toWiFiQuality(int32_t rssi) {
  unsigned int  qu;
  if (rssi <= -100) qu = 0;
  else if (rssi >= -50) qu = 100;
  else qu = 2 * (rssi + 100);
  return qu;
}

// ===== ПРЕОБРАЗОВАНИЕ STRING В FLOAT =====
float String_to_float(String s) {
  float out;
  String int_s = s.substring(0, (s.indexOf('.')));
  String fract_s = s.substring((s.indexOf('.') + 1), s.length());
  if (s.indexOf('.') != -1) { //если в строке была найдена точка
    if (fract_s.length() == 1)  out = float(int_s.toInt() + float(fract_s.toInt() * 0.1));
    if (fract_s.length() == 2)  out = float(int_s.toInt() + float(fract_s.toInt() * 0.01));
    if (fract_s.length() == 3)  out = float(int_s.toInt() + float(fract_s.toInt() * 0.001));
  }
  else out = float(s.toInt());  // если не было точки в строке
  return out;
}

// ===== ПРЕОБРАЗОВАНИЕ FLOAT В STRING =====
String float_to_String(float n) {
  String value = "";
  value = String(n);
  String str1 = value.substring(0, value.indexOf('.'));  //отделяем число до точки
  int accuracy;
  accuracy = value.length() - value.indexOf('.'); //определяем точность (сколько знаков после точки)
  int stepen = 10;
  for (int i = 0; i < accuracy; i++) stepen = stepen * 10;
  String str2 = String(n * stepen);  // приводит float к целому значению с потеряй разделителя точки(в общем удаляет точку)
  String out = "";
  out += str1 += '.';
  out += str2.substring((value.length() - accuracy), (value.length() - 1));
  out += '\n';
  return out;
}

//-----------------------------------------------------------------------------
//                            ПЕРЕЗАГРУЗКА
//-----------------------------------------------------------------------------
void reboot() {
  handleRoot();
  delay(1000);
  ESP.reset();
}

//-----------------------------------------------------------------------------
//                            WEB-СЕРВЕР
//-----------------------------------------------------------------------------
void http_server() {
  
  //*** Обслуживание главной страницы
  server.on("/", handleRoot);
  server.on("/watering_set", watering_set);
  server.on("/watering_ctrl", watering_ctrl);

  //*** Обслуживание страницы изменения режима управления и калибровки датчика
  server.on("/ctrl_mode", ctrl_mode);
  server.on("/ctrl_mode_set", ctrl_mode_set);
  server.on("/watering_adj", watering_adj);

  //*** Обслуживание страницы обновления версии ПО
  server.on("/upd", upd);
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    handleRoot();
    delay(1000);
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.setDebugOutput(true);
      WiFiUDP::stopAll();
      Serial.printf("Update: %s\n", upload.filename.c_str());
      uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
      if (!Update.begin(maxSketchSpace)) { //start with max available size
        Update.printError(Serial);
      }
    }
    else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    }
    else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) { //true to set the size to the current progress
        Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
      }
      else {
        Update.printError(Serial);
      }
      Serial.setDebugOutput(false);
    }
    yield();
  });

  // *** Перезагрузка модуля
  //server.on("/reboot", reboot);
  server.on("/reboot", HTTP_GET, []() {
    handleRoot();
    delay(1000);
    ESP.reset();
  });

  // *** Старт WEB-сервера
  server.begin();
  delay(1000);
}

// ===== ГЛАВНАЯ СТРАНИЦА. СОСТОЯНИЕ СИСТЕМЫ ПОЛИВА =====
void handleRoot() {
  String str = "";
  str += F("<!DOCTYPE HTML>");
  //str += F("<html>");
  //str += F("<head>");

  str += F("<script type='text/javascript'>");
  str += F("function sysTime() {");
  str += F("var tm=new Date();");
  str += F("var year=tm.getFullYear();");
  str += F("var month=tm.getMonth()+1;");
  str += F("var day=tm.getDate();");
  str += F("var hours=tm.getHours();");
  str += F("var minutes=tm.getMinutes();");
  str += F("var seconds=tm.getSeconds();");
  str += F("if (day <= 9) day = '0' + day;");
  str += F("if (month <= 9) month = '0' + month;");
  str += F("if (hours <= 9) hours = '0' + hours;");
  str += F("if (minutes <= 9) minutes = '0' + minutes;");
  str += F("if (seconds <= 9) seconds = '0' + seconds;");
  str += F("document.getElementById('date_txt').innerHTML=day+'.'+month+'.'+year;");
  str += F("document.getElementById('time_txt').innerHTML=hours+':'+minutes+':'+seconds;");
  str += F("t=setTimeout('sysTime()',500); }");
  str += F("</script>");

  str += F("<meta charset='UTF-8' name='viewport' content='width=device-width, initial-scale=1'>");
  str += F("<meta http-equiv='Refresh' content='60' />");
  str += F("<title>СИСТЕМА ПОЛИВА");
  str += String(n);
  str += F("</title>");
  str += F("<style type='text/css'>");

  str += F("body {");
  str += F(" text-align: center;");
  str += F(" display: inline-block;");
  str += F(" background-color: #FFFFFF;");
  str += F(" font-family: Arial, Helvetica, Sans-Serif;");
  str += F(" margin-left: 20px;");
  str += F(" Color: #000000;");
  str += F(" font-size: 12pt");
  str += F("}");

  str += F(".align_center {");
  str += F(" text-align: center;");
  str += F("}");

  str += F(".sensors_txt {");
  str += F(" display: inline-block;");
  str += F(" text-align: left;");
  str += F(" padding-left: 5px;");
  str += F(" width: 200px;");
  str += F("}");

  str += F(".ctrl_txt {");
  str += F(" display: inline-block;");
  str += F(" text-align: left;");
  str += F(" padding-left: 5px;");
  str += F(" width: 155px;");
  str += F("}");

  str += F(".set_txt {");
  str += F(" display: inline-block;");
  str += F(" text-align: left;");
  str += F(" padding-left: 5px;");
  str += F(" width: 80px;");
  str += F("}");

  str += F(".comm_txt {");
  str += F(" display: inline-block;");
  str += F(" text-align: left;");
  str += F(" padding-left: 5px;");
  str += F(" width: 115px;");
  str += F("}");

  str += F(".data_time {");
  str += F(" line-height: 36px;");
  str += F(" display: inline-block;");
  str += F(" width: 105px;");
  str += F(" position: relative;");
  str += F(" text-align: center;");
  str += F(" font-size: 100%;");
  str += F(" text-decoration: none;");
  str += F(" color: #FFF;");
  str += F(" background-color: #006778;");
  str += F(" background-image: -webkit-linear-gradient(top, #0087b0, #006778);");
  str += F(" background-image: linear-gradient(to bottom, #0087b0, #006778);");
  str += F(" border-bottom: solid 3px #004462;");
  str += F(" border-radius: 4px;");
  str += F(" box-shadow: inset 0 1px 0 rgba(255,255,255,0.2), 0 3px 2px rgba(0, 0, 0, 0.19);");
  str += F("}");

  str += F(".data_time:active {");
  str += F(" position:relative;");
  str += F(" top:2px;");
  str += F(" box-shadow: 0 0 0 0 rgba(0,0,0,0.2), 0 0 0 0 rgba(0,0,0,0.19);");
  str += F("}");

  str += F(".sensors_value {");
  str += F(" line-height: 36px;");
  str += F(" display: inline-block;");
  str += F(" width: 125px;");
  str += F(" position: relative;");
  str += F(" text-align: center;");
  str += F(" font-size: 100%;");
  str += F(" text-decoration: none;");
  str += F(" color: #FFF;");
  str += F(" background-color: #006778;");
  str += F(" background-image: -webkit-linear-gradient(top, #0087b0, #006778);");
  str += F(" background-image: linear-gradient(to bottom, #0087b0, #006778);");
  str += F(" border-bottom: solid 3px #004462;");
  str += F(" border-radius: 4px;");
  str += F(" box-shadow: inset 0 1px 0 rgba(255,255,255,0.2), 0 3px 2px rgba(0, 0, 0, 0.19);");
  str += F("}");

  str += F(".state_off{");
  str += F(" width: 170px;");
  str += F(" background-color: #E4685D;");
  str += F(" border: none;");
  str += F(" box-shadow: 0 1px 8px 0 rgba(0,0,0,0.2), 0 6px 20px 0 rgba(0,0,0,0.19);");
  str += F(" -moz-border-radius:4px;");
  str += F(" -webkit-border-radius:4px;");
  str += F(" border-radius:4px;");
  str += F(" color: white;");
  str += F(" padding: 10px 0px;");
  str += F(" text-decoration: none;");
  str += F(" display: inline-block;");
  str += F(" text-align: center;");
  str += F(" font-size: 100%;");
  str += F("}");

  str += F(".state_on  {");
  str += F(" width: 170px;");
  str += F(" background-color: #13b25e;");
  str += F(" border: none;");
  str += F(" box-shadow: 0 1px 8px 0 rgba(0,0,0,0.2), 0 6px 20px 0 rgba(0,0,0,0.19);");
  str += F(" -moz-border-radius:4px;");
  str += F(" -webkit-border-radius:4px;");
  str += F(" border-radius:4px;");
  str += F(" color: white;");
  str += F(" padding: 10px 0px;");
  str += F(" text-decoration: none;");
  str += F(" display: inline-block;");
  str += F(" text-align: center;");
  str += F(" font-size: 100%;");
  str += F("}");

  str += F(".ctrl_mode{");
  str += F(" line-height: 36px;");
  str += F(" display: inline-block;");
  str += F(" width: 170px;");
  str += F(" position: relative;");
  str += F(" text-align: center;");
  str += F(" font-size: 100%;");
  str += F(" text-decoration: none;");
  str += F(" color: #FFF;");
  str += F(" background-color: #006778;");
  str += F(" background-image: -webkit-linear-gradient(top, #0087b0, #006778);");
  str += F(" background-image: linear-gradient(to bottom, #0087b0, #006778);");
  str += F(" border-bottom: solid 3px #004462;");
  str += F(" border-radius: 4px;");
  str += F(" box-shadow: inset 0 1px 0 rgba(255,255,255,0.2), 0 3px 2px rgba(0, 0, 0, 0.19);");
  str += F("}");

  str += F(".ctrl_mode:active {");
  str += F(" position:relative;");
  str += F(" top:2px;");
  str += F(" box-shadow: 0 0 0 0 rgba(0,0,0,0.2), 0 0 0 0 rgba(0,0,0,0.19);");
  str += F("}");

  str += F(".ctrl_on  {");
  str += F(" width: 160px;");
  str += F(" background-color: #13b25e;");
  str += F(" border: none;");
  str += F(" box-shadow: 0 1px 8px 0 rgba(0,0,0,0.2), 0 6px 20px 0 rgba(0,0,0,0.19);");
  str += F(" -moz-border-radius:4px;");
  str += F(" -webkit-border-radius:4px;");
  str += F(" border-radius:4px;");
  str += F(" color: white;");
  str += F(" padding: 10px 0px;");
  str += F(" text-decoration: none;");
  str += F(" display: inline-block;");
  str += F(" text-align: center;");
  str += F(" font-size: 100%;");
  str += F("}");

  str += F(".ctrl_on:active {");
  str += F(" position:relative;");
  str += F(" top:2px;");
  str += F(" box-shadow: 0 0 0 0 rgba(0,0,0,0.2), 0 0 0 0 rgba(0,0,0,0.19);");
  str += F("}");

  str += F(".ctrl_off{");
  str += F(" width: 160px;");
  str += F(" background-color: #E4685D;");
  str += F(" border: none;");
  str += F(" box-shadow: 0 1px 8px 0 rgba(0,0,0,0.2), 0 6px 20px 0 rgba(0,0,0,0.19);");
  str += F(" -moz-border-radius:4px;");
  str += F(" -webkit-border-radius:4px;");
  str += F(" border-radius:4px;");
  str += F(" color: white;");
  str += F(" padding: 10px 0px;");
  str += F(" text-decoration: none;");
  str += F(" display: inline-block;");
  str += F(" text-align: center;");
  str += F(" font-size: 100%;");
  str += F("}");

  str += F(".ctrl_off:active {");
  str += F("  position:relative;");
  str += F("  top:2px;");
  str += F("  box-shadow: 0 0 0 0 rgba(0,0,0,0.2), 0 0 0 0 rgba(0,0,0,0.19);");
  str += F("}");

  str += F(".set_val {");
  str += F(" padding: 16px;");
  str += F(" width: 90px;");
  str += F(" position: relative;");
  str += F(" text-align: center;");
  str += F(" -webkit-transition: all 0.30s ease-in-out;");
  str += F(" -moz-transition: all 0.30s ease-in-out;");
  str += F(" -ms-transition: all 0.30s ease-in-out;");
  str += F(" outline: none;");
  str += F(" box-sizing: border-box;");
  str += F(" -webkit-box-sizing: border-box;");
  str += F(" -moz-box-sizing: border-box;");
  str += F(" background: #fff;");
  str += F(" margin-bottom: 4%;");
  str += F(" border: 1px solid #ccc;");
  str += F(" padding: 3%;");
  str += F(" color: #555;");
  str += F(" font-size: 100%;");
  str += F("}");

  str += F(".set_val:focus {");
  str += F(" box-shadow: 0 0 5px #276873;");
  str += F(" padding: 3%;");
  str += F(" border: 2px solid #276873;");
  str += F("}");

  str += F(".set_button {");
  str += F(" line-height: 36px;");
  str += F(" display: inline-block;");
  str += F(" width: 210px;");
  str += F(" position: relative;");
  str += F(" text-align: center;");
  str += F(" font-size: 100%;");
  str += F(" text-decoration: none;");
  str += F(" color: #FFF;");
  str += F(" background-color: #006778;");
  str += F(" background-image: -webkit-linear-gradient(top, #0087b0, #006778);");
  str += F(" background-image: linear-gradient(to bottom, #0087b0, #006778);");
  str += F(" border-bottom: solid 3px #004462;");
  str += F(" border-radius: 4px;");
  str += F(" box-shadow: inset 0 1px 0 rgba(255,255,255,0.2), 0 3px 2px rgba(0, 0, 0, 0.19);");
  str += F("}");

  str += F(".set_button:active {");
  str += F(" position:relative;");
  str += F(" top:2px;");
  str += F(" box-shadow: 0 0 0 0 rgba(0,0,0,0.2), 0 0 0 0 rgba(0,0,0,0.19);");
  str += F("}");

  str += F(".comm_val {");
  str += F(" line-height: 36px;");
  str += F(" display: inline-block;");
  str += F(" width: 210px;");
  str += F(" position: relative;");
  str += F(" text-align: center;");
  str += F(" font-size: 100%;");
  str += F(" text-decoration: none;");
  str += F(" color: #FFF;");
  str += F(" background-color: #006778;");
  str += F(" background-image: -webkit-linear-gradient(top, #0087b0, #006778);");
  str += F(" background-image: linear-gradient(to bottom, #0087b0, #006778);");
  str += F(" border-bottom: solid 3px #004462;");
  str += F(" border-radius: 4px;");
  str += F(" box-shadow: inset 0 1px 0 rgba(255,255,255,0.2), 0 3px 2px rgba(0, 0, 0, 0.19);");
  str += F("}");

  str += F(".comm_state {");
  str += F(" display: inline-block;");
  str += F(" width: 210px;");
  str += F(" text-align: center;");
  str += F("}");

  str += F("</style>");
  //str += F("</head>");
  str += F("<body onload='sysTime()'>");

  // *** Дата и время
  str += F("<p>Дата : ");
  str += F("<span id='date_txt'></span> г. &nbsp Время : ");
  str += F("<span id='time_txt'> </span> </p><hr/>");

  // *** Полив
  str += F("БОЛЬШАЯ ТЕПЛИЦА. ПОЛИВ  # ");
  str += String(n);
  str += F("<hr/>");

  // *** Показания датчика
  str += F("<p><font class='sensors_txt'>Влажность почвы: </font>");
  str += F("<font class='sensors_value'");
  if (moist_sensor) str += F(">");
  else str += F(" color = '#FF0000'> ");   
  str += String(moist);
  str += F(" % </font></p><hr/>");

  str += F("<div align='left'>После окончания предыдущего полива <div>");
  str += F("<p><font class='ctrl_txt'> прошло ");
  if (first_watering) str += F(" более ");
  str += F(":</font><font class='ctrl_mode' >");
  after_watering = (millis() - end_watering) / 1000; // прошло времени после окончания полива
  str += String(day(after_watering) - 1);
  str += F(" дн.");
  str += String(hour(after_watering));
  str += F("  час.");
  str += String(minute(after_watering));
  str += F("  мин.</font></p>");

  // *** Состояние клапана = ОТКРЫТ
  if (watering_state) {
    watering_cont = (millis() - start_watering) / 1000; // определение продолжительности  полива
    if (watering_state) {
      str += F("<p><font class='ctrl_txt'> От начала полива : </font><font class='ctrl_mode'>");
      if (day(watering_cont) > 1) {
        str += String(day(watering_cont) - 1);
        str += F(" дн.");
      }
      str += String(hour(watering_cont));
      str += F(" час.");
      str += digits_to_String(minute(watering_cont));
      str += F(" мин.</font></p>");
    }
    str += F("<p><font class='ctrl_txt'> Клапан полива: </font>");
    str += F("<font class='state_on'> ОТКРЫТ ");
  }

  // *** Состояние клапана = ЗАКРЫТ
  if (!watering_state) {
    watering_cont = (end_watering - start_watering) / 1000; 
    if (!first_watering) {
      str += F("<p><font class='ctrl_txt'> Длительность : </font><font class='ctrl_mode'>");
      if (day(watering_cont) > 1) {
        str += String(day(watering_cont) - 1);
        str += F(" дн.");
      }
      str += String(hour(watering_cont));
      str += F(" час.");
      str += digits_to_String(minute(watering_cont));
      str += F(" мин.</font></p>");
    }
    str += F("<p><font class='ctrl_txt'> Клапан полива: </font>");
    str += F("<font class='state_off'> ЗАКРЫТ ");
  }
  str += F("</font></p>");


  // *** Режим управления
  str += F("<p><font class='ctrl_txt'> Режим управления: </font>");
  str += F("<a href='/ctrl_mode'><button class='");
  if (mode_ctrl == 0) str += F("ctrl_mode'>АВТОМАТИЧЕСКИЙ");
  if (mode_ctrl == 1) str += F("ctrl_mode'>ДИСТАНЦИОННЫЙ");
  if (mode_ctrl == 2) str += F("ctrl_mode'>ВРУЧНУЮ");
  str += F("</button></a></p>");

  // *** Кнопки управления ВРУЧНУЮ
  if (mode_ctrl == 2) {
    str += F("<a href='/watering_ctrl?cmd=on'><button class='ctrl_on'> ОТКРЫТЬ </button></a>&nbsp&nbsp");
    str += F("<a href='/watering_ctrl?cmd=off'><button class='ctrl_off'> ЗАКРЫТЬ </button></a></p>");
  }
  str += F("<hr/>");

  // *** Уставка датчика влажности почвы для АВТОМАТИЧЕСКОГО режима
  str += F("УСТАВКА ДАТЧИКА ВЛАЖНОСТИ ПОЧВЫ <hr />");
  str += F("<form  method='POST' action='/watering_set'>");
  str += F("<p><font class='ctrl_txt'> Влажность почвы : </font>");
  str += F("<input type='TEXT' class='set_val'  name='set_moist' placeholder=' ");
  str += float_to_String(set_moist);
  str += F("\n'/><font class='set_txt'> +/- 5 % </font></p>");
  str += F("<p><font class='comm_txt'> </font><input type='SUBMIT' class='set_button' value='ПРИМЕНИТЬ ЗНАЧЕНИЕ'></p>");
  str += F("</form><hr/>");

  // *** Информация о подключениях
  str += F("ИНФОРМАЦИЯ О ПОДКЛЮЧЕНИЯХ<hr />");
  str += F("<p><font class='comm_txt'> WiFi SSID : </font>");
  str += F("<font class='comm_val'>");
  str += WiFi.SSID();
  str += F("</font></a></p>");
  if (WiFi.status() == WL_CONNECTED) {
    str += F("<p><font class='comm_txt'> IP addres: </font>");
    str += F("<font class='comm_val'>");
    str += WiFi.localIP().toString();
    str += F("</font></p>");
    str += F("<p><font class='comm_txt'> WiFi RSSI : </font>");
    if (toWiFiQuality(WiFi.RSSI()) < 30) str += F("<font class='comm_state' color = '#FF0000'> ");
    else str += F("<font class='comm_state' color = '#009900'> ");
    str += String(toWiFiQuality(WiFi.RSSI()));
    str += F(" % </font></p><hr />");

    str += F("<p><font class='comm_txt'> MQTT BROKER: </font>");
    str += F("<font class='comm_val'>");
    str += String(mqtt_server);
    str += F(" : ");
    str += String(mqtt_port);
    str += F("</font></a></p>");

    str += F("<p><font class='comm_txt'> To MQTT </font>");
    if (client.connect("BigGreenHouse")) str += F("<font class='comm_state' color = '#009900'> CONNECTED !</font></p>");
    else str += F("<font class='comm_state' color = '#FF0000'> NOT CONNECTED !!!</font></p>");
  }
  else {
    str += F("<p><font class='comm_txt'> WiFi </font>");
    str += F("<font class='comm_state' color = '#FF0000'> NOT CONNECTED !!! </font></p>");
  }
  str += F("<hr/>");

  // *** Версия ПО и обновление
  str += F("<form method='GET' action='upd'>");
  str += F("<p><font class='comm_txt'> Текущая версия ПО: </font>");
  str += F("<input type='SUBMIT' class='set_button'  value='");
  str += String(FW_VERSION);
  str += F("'/></p>");
  str += F("</form>");


  str += F("</body>");
  //str += F("</html>");
  server.send ( 200, "text/html", str );
  delay(1000);
}

// *** Управление поливом ВРУЧНУЮ  ***
void watering_ctrl() {
  if (server.arg(0) == "on") man_cmd = true;
  if (server.arg(0) == "off") man_cmd = false;
  server.send ( 200, "text/html", root_str );
  delay(1000);
}

// *** Изменить уставки датчика влажности почвы для АВТОМАТИЧЕСКОГО режима ***
void watering_set() {
  float set;
  set = String_to_float(server.arg(0));
  if (set != 0.0) set_moist = set;
  if (set_moist < 10.0)  set_moist = 10.0;
  if (set_moist > 90.0)  set_moist = 90.0;
  server.send ( 200, "text/html", root_str );
  delay(1000);
}

// ===== WEB-СЕРВЕР. ВЫБОР РЕЖИМА УПРАВЛЕНИЯ =====
void ctrl_mode() {
  String str = "";
  str += F("<!DOCTYPE HTML>");
  //str += F("<html>");
  //str += F("<head>");
  str += F("<meta charset='UTF-8' name='viewport' content='width=device-width, initial-scale=1'>");
  str += F("<title>ВЫБОР РЕЖИМА УПРАВЛЕНИЯ</title>");
  str += F("<style type='text/css'>");

  str += F("body {");
  str += F(" text-align: center;");
  str += F(" display: inline-block;");
  //str += F("  max-width: 400px;");
  str += F(" background-color: #FFFFFF;");
  str += F(" font-family: Arial, Helvetica, Sans-Serif;");
  str += F(" margin-left: 20px;");
  str += F(" Color: #000000;");
  str += F(" font-size: 12pt");
  str += F("}");

  str += F(".align_center {");
  str += F(" text-align: center;");
  str += F("}");

  str += F(".sensors_txt {");
  str += F(" display: inline-block;");
  str += F(" text-align: left;");
  str += F(" padding-left: 5px;");
  str += F(" width: 200px;");
  str += F("}");

  str += F(".ctrl_txt {");
  str += F(" display: inline-block;");
  str += F(" text-align: left;");
  str += F(" padding-left: 5px;");
  str += F(" width: 155px;");
  str += F("}");

  str += F(".set_txt {");
  str += F(" display: inline-block;");
  str += F(" text-align: left;");
  str += F(" padding-left: 5px;");
  str += F(" width: 115px;");
  str += F("}");

  str += F(".sensors_value {");
  str += F(" line-height: 36px;");
  str += F(" display: inline-block;");
  str += F(" width: 125px;");
  str += F(" position: relative;");
  str += F(" text-align: center;");
  str += F(" font-size: 100%;");
  str += F(" text-decoration: none;");
  str += F(" color: #FFF;");
  str += F(" background-color: #006778;");
  str += F(" background-image: -webkit-linear-gradient(top, #0087b0, #006778);");
  str += F(" background-image: linear-gradient(to bottom, #0087b0, #006778);");
  str += F(" border-bottom: solid 3px #004462;");
  str += F(" border-radius: 4px;");
  str += F(" box-shadow: inset 0 1px 0 rgba(255,255,255,0.2), 0 3px 2px rgba(0, 0, 0, 0.19);");
  str += F("}");

  str += F(".state {");
  str += F(" line-height: 36px;");
  str += F(" display: inline-block;");
  str += F(" width: 170px;");
  str += F(" position: relative;");
  str += F(" text-align: center;");
  str += F(" font-size: 100%;");
  str += F(" text-decoration: none;");
  str += F(" color: #FFF;");
  str += F(" background-color: #006778;");
  str += F(" background-image: -webkit-linear-gradient(top, #0087b0, #006778);");
  str += F(" background-image: linear-gradient(to bottom, #0087b0, #006778);");
  str += F(" border-bottom: solid 3px #004462;");
  str += F(" border-radius: 4px;");
  str += F(" box-shadow: inset 0 1px 0 rgba(255,255,255,0.2), 0 3px 2px rgba(0, 0, 0, 0.19);");
  str += F("}");

  str += F(".ctrl_mode{");
  str += F(" line-height: 36px;");
  str += F(" display: inline-block;");
  str += F(" width: 170px;");
  str += F(" position: relative;");
  str += F(" text-align: center;");
  str += F(" font-size: 100%;");
  str += F(" text-decoration: none;");
  str += F(" color: #FFF;");
  str += F(" background-color: #006778;");
  str += F(" background-image: -webkit-linear-gradient(top, #0087b0, #006778);");
  str += F(" background-image: linear-gradient(to bottom, #0087b0, #006778);");
  str += F(" border-bottom: solid 3px #004462;");
  str += F(" border-radius: 4px;");
  str += F(" box-shadow: inset 0 1px 0 rgba(255,255,255,0.2), 0 3px 2px rgba(0, 0, 0, 0.19);");
  str += F("}");

  str += F(".ctrl_mode:active {");
  str += F(" position:relative;");
  str += F(" top:2px;");
  str += F(" box-shadow: 0 0 0 0 rgba(0,0,0,0.2), 0 0 0 0 rgba(0,0,0,0.19);");
  str += F("}");

  str += F(".set_val {");
  str += F(" padding: 16px;");
  str += F(" width: 90px;");
  str += F(" position: relative;");
  str += F(" text-align: center;");
  str += F(" -webkit-transition: all 0.30s ease-in-out;");
  str += F(" -moz-transition: all 0.30s ease-in-out;");
  str += F(" -ms-transition: all 0.30s ease-in-out;");
  str += F(" outline: none;");
  str += F(" box-sizing: border-box;");
  str += F(" -webkit-box-sizing: border-box;");
  str += F(" -moz-box-sizing: border-box;");
  str += F(" background: #fff;");
  str += F(" margin-bottom: 4%;");
  str += F(" border: 1px solid #ccc;");
  str += F(" padding: 3%;");
  str += F(" color: #555;");
  str += F(" font-size: 100%;");
  str += F("}");

  str += F(".set_val:focus {");
  str += F(" box-shadow: 0 0 5px #276873;");
  str += F(" padding: 3%;");
  str += F(" border: 2px solid #276873;");
  str += F("}");

  str += F(".set_button {");
  str += F(" line-height: 36px;");
  str += F(" display: inline-block;");
  str += F(" width: 210px;");
  str += F(" position: relative;");
  str += F(" text-align: center;");
  str += F(" font-size: 100%;");
  str += F(" text-decoration: none;");
  str += F(" color: #FFF;");
  str += F(" background-color: #006778;");
  str += F(" background-image: -webkit-linear-gradient(top, #0087b0, #006778);");
  str += F(" background-image: linear-gradient(to bottom, #0087b0, #006778);");
  str += F(" border-bottom: solid 3px #004462;");
  str += F(" border-radius: 4px;");
  str += F(" box-shadow: inset 0 1px 0 rgba(255,255,255,0.2), 0 3px 2px rgba(0, 0, 0, 0.19);");
  str += F("}");

  str += F(".set_button:active {");
  str += F(" position:relative;");
  str += F(" top:2px;");
  str += F(" box-shadow: 0 0 0 0 rgba(0,0,0,0.2), 0 0 0 0 rgba(0,0,0,0.19);");
  str += F("}");

  str += F(".home_button {");
  str += F(" line-height: 36px;");
  str += F(" display: inline-block;");
  str += F(" width: 210px;");
  str += F(" position: relative;");
  str += F(" text-align: center;");
  str += F(" font-size: 100%;");
  str += F(" text-decoration: none;");
  str += F(" color: #FFF;");
  str += F(" background-color: #464e7a;");
  str += F(" background-image: -webkit-linear-gradient(top, #55639a, #464e7a);");
  str += F(" background-image: linear-gradient(to bottom, #55639a, #464e7a);");
  str += F(" border-bottom: solid 3px #004462;");
  str += F(" border-radius: 4px;");
  str += F(" box-shadow: inset 0 1px 0 rgba(255,255,255,0.2), 0 3px 2px rgba(0, 0, 0, 0.19);");
  str += F("}");

  str += F(".home_button:active {");
  str += F(" position:relative;");
  str += F(" top:2px;");
  str += F(" box-shadow: 0 0 0 0 rgba(0,0,0,0.2), 0 0 0 0 rgba(0,0,0,0.19);");
  str += F("}");

  str += F("</style>");
  //str += F("</head>");
  str += F("<body>");

  str += F("ВЫБОР РЕЖИМА УПРАВЛЕНИЯ<hr />");

  // *** Изменение режима управления
  str += F("<p><font class='ctrl_txt'> Режим управления: </font><font class='state'> ");
  if (mode_ctrl == 0) str += F("АВТОМАТИЧЕСКИЙ");
  if (mode_ctrl == 1) str += F("ДИСТАНЦИОННЫЙ");
  if (mode_ctrl == 2) str += F("ВРУЧНУЮ");
  str += F("</font></p><hr/>");
  str += F("<p><font class='ctrl_txt'> Изменить режим :</font><a href='/ctrl_mode_set?ctrl=rem'><button class='ctrl_mode'> ДИСТАНЦИОННЫЙ </button></a></p>");
  str += F("<p><font class='ctrl_txt'></font><a href='/ctrl_mode_set?ctrl=auto'><button class='ctrl_mode'> АВТОМАТИЧЕСКИЙ </button></a></p>");
  str += F("<p><font class='ctrl_txt'></font><a href='/ctrl_mode_set?ctrl=man'><button class='ctrl_mode'> ВРУЧНУЮ </button></a></p>");
  str += F("<hr/>");

  // *** Калибровка датчика влажности почвы
  str += F("КАЛИБРОВКА ДАТЧИКА ВЛАЖНОСТИ ПОЧВЫ <hr/>");
  str += F("<form  method='POST' action='/watering_adj'>");
  str += F("<p><font class='sensors_txt'> Выход датчика : </font>");
  str += F("<font class='sensors_value'>");
  str += sensor_moist;
  str += F(" ед. </font></p>");
  str += F("<p><font class='sensors_txt'> на воздухе (0%) &nbsp = </font>");
  str += F("<input type='TEXT' class='set_val' name='air_val' placeholder=' ");
  str += String(air_val);
  str += F("\n'/> &nbsp ед.</p>");
  str += F("<p><font class='sensors_txt'> в воде (100%) &nbsp = </font>");
  str += F("<input type='TEXT' class='set_val' name='water_val' placeholder=' ");
  str += String(water_val);
  str += F("\n'/> &nbsp ед. </p>");
  str += F("<p><font class='set_txt'> </font><input type='SUBMIT' class='set_button' value='ПРИМЕНИТЬ ЗНАЧЕНИЯ'></p>");
  str += F("</form>");

  str += F("<hr/><br/>");
  str += F("<p><a href='/'><button class='home_button'> НА ГЛАВНЫЙ ЭКРАН </button></a></p>");


  str += F("</body>");
  //str += F("</html>\n\r");
  server.send ( 200, "text/html", str );
  delay(1000);
}

// *** Изменить режим управления ***
void ctrl_mode_set() {
  if (server.arg(0) == "auto") mode_ctrl = 0;
  //if (server.arg(0)=="rem" && client.connected())  mode_ctrl = 1;
  if (server.arg(0) == "rem") mode_ctrl = 1;
  if (server.arg(0) == "man") mode_ctrl = 2;
  server.send ( 200, "text/html", root_str );
  delay(1000);
}

// *** Изменить значения калибровки датчика влажности почвы ***
void watering_adj() {
  int set;
  set = (server.arg(0)).toInt();
  if (set != 0) air_val = set;
  if (air_val > 800)  air_val = 800;
  set = (server.arg(1)).toInt();
  if (set != 0) water_val = set;
  if (water_val < 100)  water_val = 100;
  server.send ( 200, "text/html", ctrl_str );
  delay(1000);
}

// ===== ОБНОВЛЕНИЕ ПО. ВЫБОР ФАЙЛА. СТАРТ =====
void upd() {
  String str = "";
  str += F("<!DOCTYPE HTML>");
  str += F("<meta charset='UTF-8' name='viewport' content='width=device-width, initial-scale=1'/>");
  str += F("<title>ОБНОВЛЕНИЕ ПО</title>");
  str += F("<style type='text/css'>");

  str += F("body {");
  str += F(" display: inline-block;");
  str += F(" width: 320px;");
  str += F(" text-align: center;");
  str += F(" background-color: #FFFFFF;");
  str += F(" font-family: Arial, Helvetica, Sans-Serif;");
  str += F(" margin-left: 20px;");
  str += F(" Color: #000000;");
  str += F(" font-size: 12pt");
  str += F("}");

  str += F(".set_button {");
  str += F(" line-height: 36px;");
  str += F(" display: inline-block;");
  str += F(" width: 210px;");
  str += F(" position: relative;");
  str += F(" text-align: center;");
  str += F(" font-size: 100%;");
  str += F(" text-decoration: none;");
  str += F(" color: #FFF;");
  str += F(" background-color: #006778;");
  str += F(" background-image: -webkit-linear-gradient(top, #0087b0, #006778);");
  str += F(" background-image: linear-gradient(to bottom, #0087b0, #006778);");
  str += F(" border-bottom: solid 3px #004462;");
  str += F(" border-radius: 4px;");
  str += F(" box-shadow: inset 0 1px 0 rgba(255,255,255,0.2), 0 3px 2px rgba(0, 0, 0, 0.19);");
  str += F("}");

  str += F(".set_button:active {");
  str += F(" position:relative;");
  str += F(" top:2px;");
  str += F(" box-shadow: 0 0 0 0 rgba(0,0,0,0.2), 0 0 0 0 rgba(0,0,0,0.19);");
  str += F("}");

  str += F("</style>");
  
  str += F("<body>");
  str += F("<h2>ОБНОВЛЕНИЕ ПО</h2><hr/>");
  str += F("<form  method='POST' action='/update' enctype='multipart/form-data'>");
  str += F("<p><input type='FILE' name='update' /></p><br/>");
  str += F("<p><input type='SUBMIT' class='set_button' value='START' /></p>");
  str += F("</form>");
  server.sendHeader("Connection", "close");
  server.send(200, "text/html", str);
}

//-----------------------------------------------------------------------------
//                           MQTT CALLBACK
// эта функция выполняется, когда какой-то девайс публикует сообщение в топик,
// на который есть подписка
//-----------------------------------------------------------------------------
void callback(char* top, byte* payload, unsigned int length) {

  // *** Сообщение прибыло в топик
  String topic(top);
  String message = "";
  for (int i = 0 ; i < length; i++) {
    message += (char)payload[i];
  }
  
  //Serial.print(" - В топик : ");
  //Serial.print(topic);
  //Serial.print(" получено сообщение :"); 
  //Serial.print(message);
  //Serial.print(" длиной = ");
  //Serial.println(length);
 
  // *** Команда дистанционного режима управления
  if (topic == "BigGreenHouse/watering_" + String(n) + "/ctrl/cmd") {
    if (message == "1" || message == "true" || message == "start") rem_cmd = true;
    if (message == "0" || message == "false" || message == "stop") rem_cmd = false;
  }
   
  // *** Изменение режима управления
  if (topic == "BigGreenHouse/watering_" + String(n) + "/ctrl/mode") {
    if (message == "1" || message == "rem") mode_ctrl = 1;
    if (message == "0" || message == "auto") mode_ctrl = 0;
  }
}

//-----------------------------------------------------------------------------
//                           MQTT PUBLISH
// функция публикации сообщений на MQTT брокере по одному сообщению за цикл
//-----------------------------------------------------------------------------

void publishData() {
  static char message[10];
  if (mqtt_send) {
    if (step_mqtt_send == 0) event_mqtt_publish = millis();

    // *** ПУБЛИКАЦИЯ ПО ОДНОМУ СООБЩЕНИЮ ЗА ЦИКЛ
    switch (step_mqtt_send) {
      
      // *** Состояние выхода управления реле клапана           
      case 0:
        topic = "BigGreenHouse/watering_" + String(n) + "/status/cmd";
        String(valve_ctrl()).toCharArray(message, 10);
        //if (!valve_ctrl()) message = "false";
        //if (valve_ctrl()) message = "true";
        //client.publish(topic.c_str(), String(valve_ctrl()).c_str());
        break;
      
      // *** Актуальный режим управления       
      case 1:
       topic = "BigGreenHouse/watering_" + String(n) + "/status/mode";
       String(mode_ctrl).toCharArray(message, 10);
       //if (mode_ctrl == 0) message = "auto";
       //if (mode_ctrl == 1) message = "rem";
        //if (mode_ctrl == 1) message = "man";
        //client.publish(topic.c_str(), String(mode_ctrl).c_str());
        break;

     // *** Показания датчика влажности почвы      
     case 2:      
       topic = "BigGreenHouse/watering_" + String(n) + "/status/moisture";
        dtostrf(moist, 8, 2, message); 
        //client.publish(topic.c_str(), message);
        break;
      
      // *** Показания датчика ...      
      case 3:
        //topic = "BigGreenHouse/watering_" + String(n) + "/status/...";
       //dtostrf(..., 8, 2, message); 
       //client.publish(topic.c_str(), message);
       break;

      // *** Показания датчика ...      
      case 4:
        //topic = "BigGreenHouse/watering_" + String(n) + "/status/...";
        //dtostrf(..., 8, 2, message); 
        //client.publish(topic.c_str(), message);
        break;  

      // *** Статус датчика влажности почвы       
      case 5:
        topic = "BigGreenHouse/watering_" + String(n) + "status/moisture_sensor";
        String(moist_sensor).toCharArray(message, 10);
        //if (!moist_sensor) message = "error";
        //if (moist_sensor) message = "ok";
        //client.publish(topic.c_str(), String(moist_sensor).c_str());
        break;
    }
    
    client.publish(topic.c_str(), message);
    
    step_mqtt_send += 1;
    if (step_mqtt_send > 5)  {
      mqtt_send = false;
      step_mqtt_send = 0;
    }
  }
}

//-----------------------------------------------------------------------------
//                ПОДКЛЮЧЕНИЕ К WiFi и MQTT. ПОДПИСКА НА ТОПИКИ
//-----------------------------------------------------------------------------
void connect() {
  event_reconnect = millis();

  // *** ЕСЛИ ОТСУТСТВУЕТ ПОДКЛЮЧЕНИЕ К WiFi РОУТЕРУ - ПОДКЛЮЧИТЬСЯ
  if (WiFi.status() != WL_CONNECTED) {    // WiFi.status() == 3 : WL_CONNECTED
    if (!sta_ok) sta_wifi();              // подключиться к роутеру, если ранее подключение к роутеру отсутствовало
    else WiFi.softAP(ap_ssid, ap_pass);   // включить точку доступа, если соединение с роутером отсутствует
  }
  else if (WiFi.localIP() == ip) {        // иначе, если IP адрес известен
    WiFi.softAPdisconnect(true);          // отключить точку доступа ESP
  }

  // *** ЕСЛИ WiFi ЕСТЬ, НО ОТСУТСТВУЕТ ПОДКЛЮЧЕНИЕ К БРОКЕРУ - ПОДКЛЮЧИТЬСЯ
  if (WiFi.isConnected() && !client.connected()) {
    Serial.print("Attempting MQTT connection...");  // Попытка подключиться к MQTT-брокеру...

    // *** ПОСЛЕ ПОДКЛЮЧЕНИЯ К БРОКЕРУ ПОДПИСАТЬСЯ НА ТОПИК(-И)
    //     функция client.connect(clientID) выполняет подключение clientID к брокеру
    //     возвращает : false = ошибка подключения / true = соединение выполнено успешно.
    if (client.connect(mqtt_client.c_str())) {
      Serial.println("MQTT connected OK");
      client.setCallback(callback);

      // *** Подписка на топик с командой управления
      topic = "BigGreenHouse/watering_" + String(n) + "/ctrl/cmd";
      client.subscribe(topic.c_str());  // подписка на топики

      // *** Подписка на топик с режимом управления
      topic = "BigGreenHouse/watering_" + String(n) + "/ctrl/mode";
      client.subscribe(topic.c_str());  // подписка на топики
      
      //client.subscribe("BigGreenHouse/....
      // ....
    }
    else {
      Serial.print("MQTT connected failed, rc=");     // подключение к брокеру отсутствует
      Serial.println(client.state());
    }
  }
}


// ===== РЕЖИМ WiFi_STA. НАСТРОЙКА И ПОДКЛЮЧЕНИЕ К РОУТЕРУ =====
void sta_wifi() {
  WiFi.disconnect();
  IPAddress dns2(8, 8, 8, 8);

  // *** STA WiFi - настройки и подключение к роутеру ***
  if (WiFi.getAutoConnect() != true) {    //configuration will be saved into SDK flash area
    WiFi.setAutoConnect(true);            //on power-on automatically connects to last used hwAP
    WiFi.setAutoReconnect(true);          //automatically reconnects to hwAP in case it's disconnected
  }
  delay(100);

  // - Keenetic-2927 -
  if (WiFi.status() != WL_CONNECTED) {
    ssid = "...";
    pass = "...";
    ip = IPAddress(192, 168, 123, 20+n);
    gw = IPAddress(192, 168, 123, 1);
    mask = IPAddress(255, 255, 255, 0);
    mqtt_server = "192.168.123.222";
    mqtt_port = 1883;
    connect_wifi();
  }

  // - WiHome -
  if (WiFi.status() != WL_CONNECTED) {
    ssid = "...";
    pass = "...";
    ip = IPAddress(192, 168, 50, 20+n);
    gw = IPAddress(192, 168, 50, 1);
    mask = IPAddress(255, 255, 255, 0);
    mqtt_server = "192.168.50.222";
    mqtt_port = 1883;
    connect_wifi();
  }

  if (WiFi.status() == WL_CONNECTED) {
    sta_ok = true;                            // подключение к роутеру состоялось
    if (WiFi.localIP() == ip) {
       WiFi.softAPdisconnect(true);            // отключить точку доступа ESP
    }
    client.setServer(mqtt_server, mqtt_port); // инициализация MQTT брокера

    Serial.print("Connected to ");  // Подключено к
    Serial.println(WiFi.SSID());
    Serial.print("IP address : ");  // IP-адрес :
    Serial.println(WiFi.localIP());
  }
}

// ===== ПОДКЛЮЧЕНИЕ К WiFi РОУТЕРУ =====
void connect_wifi() {
  Serial.print("To WiFi : ");
  Serial.println(ssid);
  WiFi.config(ip, gw, mask, gw);
  WiFi.begin(ssid.c_str(), pass.c_str()); // инициализация подключения к роутеру
  byte tries = 10;
  while (--tries &&  WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  Serial.println("");
  Serial.print("WiFi.status = ");
  Serial.println(WiFi.status());
}

//-----------------------------------------------------------------------------
//  ОБРАБОТКА ПРЕРЫВАНИЯ ОТ ТАЙМЕРА ПОЛИВА - ЛОКАЛЬНАЯ КОМАНДА ОТКРЫТЬ КЛАПАН
//-----------------------------------------------------------------------------
void ICACHE_RAM_ATTR ISR_tmr_opn() {
  if (!digitalRead(tmr_opn) && digitalRead(tmr_cls))   auto_cmd = true;
  //auto_cmd = (!digitalRead(tmr_opn) && digitalRead(tmr_cls))
}

//-----------------------------------------------------------------------------
//  ОБРАБОТКА ПРЕРЫВАНИЯ ОТ ТАЙМЕРА ПОЛИВА - ЛОКАЛЬНАЯ КОМАНДА ЗАКРЫТЬ КЛАПАН
//-----------------------------------------------------------------------------
void ICACHE_RAM_ATTR ISR_tmr_cls() {
  if (!digitalRead(tmr_cls) && digitalRead(tmr_opn))   auto_cmd = false;
  //auto_cmd = !(!digitalRead(tmr_cls) && digitalRead(tmr_opn))
}

//-----------------------------------------------------------------------------
//                          СТАРТОВАЯ ИНИЦИАЛИЗАЦИЯ
//-----------------------------------------------------------------------------
void setup() {
  // *** SERIAL PORT
  Serial.begin(115200);

  // *** EEPROM
  EEPROM.begin(512);
  int i;
  unsigned char k;
  char ch[100];
  int len = 0;
  while (k != '\0' && len < 100) {
    EEPROM.get(0 + len, k);
    ch[len] = k;
    len++;
  }
  ch[len] = '\0';
  eeprom_ok = (String(ch) == ap_ssid);

  Serial.println ("=== EEPROM ===");
  Serial.println (String(ch));
  Serial.println (ap_ssid);
  Serial.println (eeprom_ok);


  // *** GPIO MODE
  pinMode(valve, OUTPUT);
  pinMode(ESP_BUILTIN_LED, OUTPUT);
  pinMode(tmr_opn, INPUT_PULLUP); // ожидаение импульса низкого уровеня
  pinMode(tmr_cls, INPUT_PULLUP); // ожидаение импульса низкого уровеня
  pinMode(tmr_rst, INPUT);       // вывод для рестарта таймера

  // *** GPIO STATE
  //digitalWrite(tmr_rst, HIGH);
  digitalWrite(valve, LOW); // ... или восстановить состояние из EEPROM ?
  digitalWrite(ESP_BUILTIN_LED, LOW);
  attachInterrupt(digitalPinToInterrupt(tmr_opn), ISR_tmr_opn, CHANGE);  // активировать прерывание от таймера полива
  attachInterrupt(digitalPinToInterrupt(tmr_cls), ISR_tmr_cls, CHANGE);  // активировать прерывание от таймера полива

  // *** VARIABLE STATE
  if (eeprom_ok) {
    byte state;
    EEPROM.get(114, mode_ctrl);        // режим управления

    EEPROM.get(110, state);
    man_cmd = bitRead(state, 0);  // команда ручного режима для реле клапана полива
    rem_cmd = bitRead(state, 1);  // дистанционная команда для реле клапана полива
    auto_cmd = bitRead(state, 2); // локальная команда для реле клапана полива
    watering_state = bitRead(state, 3); // состояние полива

    EEPROM.get(128, set_moist);   // уставка влажности почвы для отключения полива
    EEPROM.get(132, air_val);     // уставка 0% для датчика влажности почвы
    EEPROM.get(136, water_val);     // уставка 100% для датчика влажности почвы
  }
  else {
    mode_ctrl = 0;         // режим: auto
    rem_cmd = false;  // дистанционная команда для реле клапана полива = выключить
    auto_cmd = false; // локальная команда для реле клапана полива = выключить
    man_cmd = false;  // команда ручного режима для реле клапана полива = выключить
    watering_state = false;
  }
  //watering_state = false;
  first_watering = true;

  // *** ИНИЦИАЛИЗАЦИЯ ОПРОСА АНАЛОГОВОГО ДАТЧИКА
  sensors_read();

  // *** ИНИЦИАЛИЗАЦИЯ ТОЧКИ ДОСТУПА (WiFi_AP)
  WiFi.hostname(ap_ssid);     // DHCP Hostname (useful for finding device for static lease)
  WiFi.softAPConfig (ap_ip, ap_gateway, ap_subnet);
  WiFi.softAP(ap_ssid, ap_pass);
  if (WiFi.getPersistent() == true) WiFi.persistent(false);
  //sta_wifi();

  // *** ПОДКЛЮЧЕНИЕ К РОУТЕРУ (WiFi_STA) И MQTT БРОКЕРУ
  mqtt_client = ap_ssid;
  connect();

  // *** WEB-СЕРВЕР
  http_server();

  Serial.println("Ready");  //  "Готово"
  digitalWrite(ESP_BUILTIN_LED, HIGH);

  // *** ИНИЦИАЛИЗАЦИЯ ТАЙМЕРОВ СОБЫТИЙ
  mem = now();
  event_serial_output = millis();
  //watering_ok = false;
}

//-----------------------------------------------------------------------------
//                                ОСНОВНОЙ ЦИКЛ
//-----------------------------------------------------------------------------
void loop() {
  // *** режимы управления
  if (mode_ctrl > 2) mode_ctrl = 2;
  if (mode_ctrl < 0) mode_ctrl = 0;

  if (mode_ctrl != 2) man_cmd = valve_ctrl(); // "безударный" переход в ручной режим

  // *** опрос датчика влажности почвы и коррекция команды автономного режима 
  if ((millis() - event_sensors_read) >= 60000) sensors_read();

  // *** включение реле клапана
  digitalWrite(valve, valve_ctrl());
  
  // *** учет времени полива
  if (digitalRead(valve) && !watering_state) {  // старт полива
    start_watering = millis();
    watering_state = true;
  }

  if (!digitalRead(valve) && watering_state) {  // окончание полива
    end_watering = millis();
    watering_state = false;
    first_watering = false;  // сброс при окончании первого после рестарта полива
  }

  // *** обработка запросов к WEB-серверу
  server.handleClient();

  // *** клиент MQTT
  if (client.connected()) {
    client.loop();
    if ((millis() - event_mqtt_publish) >= 10000) mqtt_send = true;
    publishData();
  }

  // *** проверка подключения к WiFi и МQTT брокеру и, при необходимости, переподключение
  else if ((millis() - event_reconnect) >= 60000) connect();

  // *** вывод информации в порт
  if (millis() - event_serial_output > 20000) { // повторять через 20 секунд
    event_serial_output = millis();
    Serial.print("Режим управления :");
    switch (mode_ctrl) {
      case 0:
        Serial.println("АВТОНОМНЫЙ");
        break;
      case 1:
        Serial.println("ДИСТАНЦИОННЫЙ");
        break;
      case 2:
        Serial.println("ВРУЧНУЮ");
        break;
    }
    WiFi.printDiag(Serial);
    Serial.print("WiFi status = ");
    switch (WiFi.status()) {
      case 0:
        Serial.println("0: WL_IDLE_STATUS – WiFi в процессе между сменой статусов");
        break;
      case 1:
        Serial.println("1: WL_NO_SSID_AVAIL – заданный SSID находится вне зоны доступа");
        break;
      case 2:
        Serial.println("2: ????");
        break;
      case 3:
        Serial.println("3: WL_CONNECTED – успешно подключен к WiFi");
        break;
      case 4:
        Serial.println("4: WL_CONNECT_FAILED – задан неправильный пароль");
        break;
      case 5:
        Serial.println("5: ????");
        break;
      case 6:
        Serial.println("6: WL_DISCONNECTED – ESP8266 не в режиме станции");
        break;
    }
    String state = "";
    state += F("Присвоен IP адрес: ");
    state += WiFi.localIP().toString();
    Serial.println(((WiFi.status() == WL_CONNECTED)) ? state : "Не подключен к роутеру");
    Serial.print("MAC: ");
    Serial.println(WiFi.macAddress());
  }

  // *** Обновление значений в EEPROM
  char charBuf[100];
  ap_ssid.toCharArray(charBuf, 100);
  EEPROM.put(0, charBuf);
  EEPROM.put(114, mode_ctrl);        // режим управления
  byte state = 0;
  //if (man_cmd) state += 1;
  //if (rem_cmd) state += 2;
  //if (auto_cmd) state += 4;
  bitWrite(state, 0, man_cmd);        // команда ручного режима для реле клапана полива
  bitWrite(state, 1, rem_cmd);        // дистанционная команда для реле клапана полива
  bitWrite(state, 2, auto_cmd);       // локальная команда для реле клапана полива
  bitWrite(state, 3, watering_state); // состояние полива
  EEPROM.put(110, state);
  EEPROM.put(128, set_moist);         // уставка влажности почвы
  EEPROM.put(132, air_val);           // уставка 0% для датчика влажности почвы
  EEPROM.put(136, water_val);         // уставка 100% для датчика влажности почвы
  //boolean result =  EEPROM.commit();
  if (!EEPROM.commit()) Serial.println("ERROR! EEPROM commit failed");
  delay(100);
}
