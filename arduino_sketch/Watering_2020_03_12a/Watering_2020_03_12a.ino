#define FW_VERSION "FW 12.03.2020"

// === Library === 
#include <ipaddress.h>
#include <TimeLib.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h> 
#include <PubSubClient.h>
#include <Wire.h>
#include <BME280I2C.h>

// === NUMBER ===
byte N = 1;

// === WiFi AP Mode === 
IPAddress ap_ip(192,168,123,(20+N));
IPAddress ap_gateway(192,168,123,(20+N));
IPAddress ap_subnet(255,255,255,0);
String ap_ssid = "BGH_watering_"+String(N);
String ap_pass = "12345678";

// === WiFi STA Mode === 
String ssid, pass;         
boolean sta_ok = false;   // подключение к роутеру состоялось

// === MQTT broker === 
char* mqtt_server;
int mqtt_port;
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
int analog_sensor;
int air_val = 520;  // показания датчика на воздухе, соответствуют 0%
int water_val = 260;  // показания датчика в воде, соответствуют 100%
float moist;
float set_moist = 65.0; // оптимальная влажность почвы 60...70% или более

// === GPIO ===
const byte tmr_rst = 16;
const byte tmr_opn = 12;
const byte tmr_cls = 14;
const byte valve = 13;
const byte ESP_BUILTIN_LED = 2;

// === CTRL - управление ===
byte last_watering_day, last_watering_hour, last_watering_minute;
byte ctrl = 0;                        // режим: 0 = auto, 1 = remote, 2 = manual
boolean first_watering = false;
boolean watering_run = false;         // состоялся полив после перезагрузки 
boolean man_valve = false;
boolean rem_valve = false;            // дистанционная команда для реле клапана полива: false = выключить, true = включить
volatile boolean auto_valve = false;  // автоматическая команда для реле клапана полива: false = выключить, true = включить

// === ТАЙМЕРЫ СОБЫТИЙ ===
long eventTime; // для снятия текущего времени с начала старта
long event_reconnect;  // для переподключения
long event_sensors_read; // для опроса датчиков
//long event_bme_begin; // для подключения  датчика BME280
long event_serial_output;
long event_mqtt_publish;
long start_watering; // старт полива
long end_watering; // окончание полива
time_t mem;

// ===== ПРОВЕРКА ДОСТУПНОСТИ WiFi СЕТИ ======
boolean scanWIFI(String found){
  uint8_t n = WiFi.scanNetworks();
  for (uint8_t i = 0; i < n; i++) {
    String ssidMy = WiFi.SSID(i);
    if (ssidMy == found) return true; 
  }
  return false;
}

//-----------------------------------------------------------------------------
//                    КОМАНДА УПРАВЛЕНИЯ РЕЛЕ КЛАПАНА ПОЛИВА
//-----------------------------------------------------------------------------
boolean valve_ctrl() {
  boolean out = ((ctrl==0) && auto_valve) || ((ctrl==1) && rem_valve) || ((ctrl==2) && man_valve);
  if(out) watering_run = true;
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
float String_to_float(String s){
  float out;
  String int_s = s.substring(0, (s.indexOf('.')));
  String fract_s = s.substring((s.indexOf('.') + 1), s.length());
  if(s.indexOf('.') != -1) {  //если в строке была найдена точка
    if(fract_s.length() == 1)  out = float(int_s.toInt() + float(fract_s.toInt() * 0.1)); 
    if(fract_s.length() == 2)  out = float(int_s.toInt() + float(fract_s.toInt() * 0.01));
    if(fract_s.length() == 3)  out = float(int_s.toInt() + float(fract_s.toInt() * 0.001));
  }
  else out = float(s.toInt());  // если не было точки в строке
  return out;
}

// ===== ПРЕОБРАЗОВАНИЕ FLOAT В STRING =====
String float_to_String(float n){
  String ansver = "";
  ansver = String(n);
  String ansver1 = ansver.substring(0, ansver.indexOf('.'));  //отделяем число до точки
  int accuracy;
  accuracy = ansver.length() - ansver.indexOf('.'); //определяем точность (сколько знаков после точки)
  int stepen = 10;
  for(int i = 0; i < accuracy; i++) stepen = stepen * 10;  
  String ansver2 = String(n * stepen);  // приводит float к целому значению с потеряй разделителя точки(в общем удаляет точку)
  String ansver3 = "";
  ansver3 += ansver1 += '.';
  ansver3 += ansver2.substring((ansver.length() - accuracy), (ansver.length() - 1));
  ansver3 += '\n';
  return ansver3;   
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
  server.on("/", handleRoot); 
  server.on("/watering_set", watering_set);
  server.on("/watering_ctrl", watering_ctrl);

  server.on("/ctrl_mode", ctrl_mode);
  server.on("/ctrl_mode_set", ctrl_mode_set);
     
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

  //server.on("/reboot", reboot);
  server.on("/reboot", HTTP_GET, []() {
    handleRoot();
    delay(1000);
    ESP.reset(); 
  });
    
  server.begin();
  delay(1000);
}

// ===== ГЛАВНАЯ СТРАНИЦА. СОСТОЯНИЕ СИСТЕМЫ ПОЛИВА =====
void handleRoot(){
  String str = "";
  str += F("<!DOCTYPE HTML>");
  str += F("<html>");
  str += F("<head>");
  
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
  str += String(N);
  str += F("</title>");
  str += F("<style type='text/css'>");
  
  str += F("body {");   
  str += F(" text-align: center;");
  str += F(" display: inline-block;");
  //str += F(" max-width: 600px;");
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
  str += F("</head>");
  //str += F("<body>");
  str += F("<body onload='sysTime()'>");
  

  // *** Дата и время
  str += F("<p>Дата : ");
  str += F("<span id='date_txt'></span> г. &nbsp Время : "); 
  str += F("<span id='time_txt'> </span> </p><hr/>");

  // *** Полив 
  str += F("<div class='align_center'><h2>ПОЛИВ");
  str += String(N);
  str += F("В БОЛЬШОЙ ТЕПЛИЦЕ</h2></div><hr />");
  
  // *** Показания датчика 
 /*
  str += F("<p><font class='sensors_txt'>Влажность почвы: </font>"); 
  str += F("<font class='sensors_val'>");
  str += String(moist);
  str += F(" % </font></p>");
  */
  
  str += F("<p>После окончания предыдущего полива </p>");
  str += F("<p><font class='ctrl_txt'> прошло: </font><font class='ctrl_mode' >"); 
  if(first_watering) str += F(" неизвестно </font></p>");  
  else {
    after_watering = (millis() - end_watering)/1000; // прошло времени после окончания полива
    str += String(day(after_watering)-1);
    str += F(" дн.");
    str += String(hour(after_watering));
    str += F("  час.");  
    str += String(minute(after_watering));
    str += F("  мин.</font></p>");
  }

  // *** клапан ОТКРЫТ
  if(watering_state) {  
    watering_cont = (millis() - start_watering)/1000; // определение продолжительности  полива
    if(!first_watering) {
      str += F("<p><font class='ctrl_txt'> От начала полива : </font><font class='ctrl_mode'>");
      if (day(watering_cont) > 1){
        str += String(day(watering_cont)-1);
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

  // *** клапан ЗАКРЫТ
  if(!watering_state) {  
    watering_cont = (end_watering - start_watering)/1000; // определение продолжительности  полива ==== !!!!!! УТОЧНИТЬ !!!!!!
    if(!first_watering) {
      str += F("<p><font class='ctrl_txt'> Продолжительность : </font><font class='ctrl_mode'>");
      if (day(watering_cont) > 1){
        str += String(day(watering_cont)-1);
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
  str += F("</font></p><br />"); 


  // *** Режим управления
  str += F("<p><font class='ctrl_txt'> Режим управления: </font>");
  str += F("<a href='/ctrl_mode'><button class='");
  if(ctrl == 0) str += F("ctrl_mode'>АВТОМАТИЧЕСКИЙ");
  if(ctrl == 1) str += F("ctrl_mode'>ДИСТАНЦИОННЫЙ");
  if(ctrl == 2) str += F("ctrl_mode'>ВРУЧНУЮ");
  str += F("</button></a></p>"); 

  // *** Кнопки управления ВРУЧНУЮ
  if (ctrl == 2) {
    str += F("<a href='/watering_ctrl?cmd=on'><button class='ctrl_on'> ОТКРЫТЬ </button></a>&nbsp");
    str += F("<a href='/watering_ctrl?cmd=off'><button class='ctrl_off'> ЗАКРЫТЬ </button></a></p>");
  }
  str += F("<hr/>");

  // *** Уставка датчика влажности для АВТОМАТИЧЕСКОГО режима
  str += F("УСТАВКА ДАТЧИКА ВЛАЖНОСТИ ПОЧВЫ
  ДЛЯ АВТОМАТИЧЕСКОГО РЕЖИМА<hr />");
  str += F("<form  method='POST' action='/watering_set'>");
  str += F("<p><font class='set_txt'> МАХ влажность </font>");
  str += F("<input type='TEXT' class='set_val'  name='set_moist' placeholder=' "); 
  str += float_to_String(set_moist);
  str += F("\n'/><font class='set_txt'> % </font></p>");
  str += F("<p><font class='set_txt'> </font><input type='SUBMIT' class='set_button' value='ПРИМЕНИТЬ ЗНАЧЕНИЕ'></p>");
  str += F("</form><hr/>");

  // *** Информация о подключениях
  str += F("<div class='align_center'>ИНФОРМАЦИЯ О ПОДКЛЮЧЕНИЯХ</div><hr />");
  str += F("<p><font class='comm_txt'> WiFi SSID : </font>");
  str += F("<font class='comm_val'>");
  str += WiFi.SSID();
  str += F("</font></a></p>");
  if(WiFi.status() == WL_CONNECTED) {
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
    if(client.connect("BigGreenHouse")) str += F("<font class='comm_state' color = '#009900'> CONNECTED !</font></p>");
    else str += F("<font class='comm_state' color = '#FF0000'> NOT CONNECTED !!!</font></p>");
  }
  else {
    str += F("<p><font class='comm_txt'> WiFi </font>");
    str += F("<font class='comm_state' color = '#FF0000'> NOT CONNECTED !!! </font></p>");
  }
  str += F("<hr/>");     
 
  // *** Версия ПО и обновление 
  str += F("<form method='GET' action='upd'>");
  str += F("<p><font class='ctrl_txt'> Текущая версия ПО: </font>");
  str += F("<input type='SUBMIT' class='ctrl_mode'  value='");
  str += String(FW_VERSION);
  str += F("'/></p>"); 
  str += F("</form>");
  

  str += F("</body>");
  //str += F("</html>\n\r");
  str += F("</html>");
  server.send ( 200, "text/html", str );
  delay(1000); 
}

// *** Управление вентиляцией ВРУЧНУЮ  ***
void watering_ctrl() {
  if (server.arg(0)=="on") man_fan = true;  
  if (server.arg(0)=="off") man_fan = false; 
  server.send ( 200, "text/html", root_str );   
  delay(1000);
}

// *** Изменить уставки АВТОМАТИЧЕСКОГО режима ***
void watering_set(){
  set_temp = String_to_float(server.arg(0));  
  if (set_temp < 21.0)  set_temp = 21.0;
  set_hum = String_to_float(server.arg(1)); 
  if (set_hum < 40.0)  set_hum = 40.0;
  if (set_hum > 90.0)  set_hum = 90.0;
  Serial.print(F("set_temp = "));
  Serial.println(set_temp);
  Serial.print(F("set_hum = "));
  Serial.println(set_hum);
  server.send ( 200, "text/html", root_str ); 
  delay(1000);
}

// ===== WEB-СЕРВЕР. ВЫБОР РЕЖИМА УПРАВЛЕНИЯ =====
void ctrl_mode(){
  String str = "";
  str += F("<!DOCTYPE HTML>");
  str += F("<html>");
  str += F("<head>");
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
  
  str += F(".ctrl_txt {");
  str += F(" display: inline-block;");
  str += F(" text-align: left;");
  str += F(" padding-left: 5px;");
  str += F(" width: 155px;");
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
  str += F("</head>");
  str += F("<body>");
  
  str += F("<div class='align_center'><h2>ВЫБОР РЕЖИМА УПРАВЛЕНИЯ</h2></div><hr />");
  str += F("<p><font class='ctrl_txt'> Режим управления: </font><font class='state'> ");
  if(ctrl == 0) str += F("АВТОМАТИЧЕСКИЙ");
  if(ctrl == 1) str += F("ДИСТАНЦИОННЫЙ");
  if(ctrl == 2) str += F("ВРУЧНУЮ");
  str += F("</font></p><hr/>");
  //str += F("<p><font class='ctrl_txt'> Изменить режим :</font>");
  //str +=F("<div class='align_center'>");
  str += F("<p><font class='ctrl_txt'> Изменить режим :</font><a href='/ctrl_mode_set?ctrl=rem'><button class='ctrl_mode'> ДИСТАНЦИОННЫЙ </button></a></p>");  
  str += F("<p><font class='ctrl_txt'></font><a href='/ctrl_mode_set?ctrl=auto'><button class='ctrl_mode'> АВТОМАТИЧЕСКИЙ </button></a></p>");
  str += F("<p><font class='ctrl_txt'></font><a href='/ctrl_mode_set?ctrl=man'><button class='ctrl_mode'> ВРУЧНУЮ </button></a></p>");
  str += F("<hr/><br/>");
  str += F("<p><a href='/'><button class='home_button'> НА ГЛАВНЫЙ ЭКРАН </button></a></p>");
  //str += F("</div>");         
  str += F("</body>");
  str += F("</html>\n\r");
  server.send ( 200, "text/html", str );
  delay(1000); 
}

// *** Изменить режим управления ***
void ctrl_mode_set() {
  if (server.arg(0)=="auto") ctrl = 0;
  if (server.arg(0)=="rem")  ctrl = 1;
  if (server.arg(0)=="man") ctrl = 2;
  server.send ( 200, "text/html", root_str );
  delay(1000);
}

// ===== ОБНОВЛЕНИЕ ПО. ВЫБОР ФАЙЛА. СТАРТ =====
void upd() {
  String str = "";
  str += F("<!DOCTYPE HTML>");
  str += F("<html>");
  //str += F("<head>");
  str += F("<meta charset='UTF-8' name='viewport' content='width=device-width, initial-scale=1'/>");
  //str += F("<meta charset='UTF-8'>");
  str += F("<title>ОБНОВЛЕНИЕ ПО</title>");
  str += F("<style type='text/css'>");

  str += F("body {"); 
  str += F(" display: inline-block;");
  //str += F(" max-width: 600px;");
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
  //str += F("</head>");
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
void callback(char* topic, byte* payload, unsigned int length) {

  // *** Сообщение прибыло в топик 
  Serial.print("Message arrived on topic: ");  
  Serial.print(topic);
  Serial.print(". Message: ");  //  ". Сообщение: "
  String messageTemp;
  for (int i=0;i<length;i++) {
    Serial.print((char)payload[i]);
    messageTemp += (char)payload[i];
  }
  Serial.println();

  // *** Обработка сообщения 
  if(topic=="BigGreenHouse/watering...."){
    // ...
    // Serial.print("Changing GPIO 4 to ");
    // "Смена состояния GPIO-контакта 4 на "
    //if(messageTemp == "1"){
    //  digitalWrite(ledGPIO4, HIGH);
    //  Serial.print("On");
    //}
    //else if(messageTemp == "0"){
    //  digitalWrite(ledGPIO4, LOW);
    //  Serial.print("Off");
    //}
    // ...
  } 
}

//-----------------------------------------------------------------------------
//                           MQTT PUBLISH
// функция публикации сообщений на MQTT брокере по одному сообщению за цикл
//-----------------------------------------------------------------------------

void publishData() {
  if(mqtt_send) {
    if (step_mqtt_send == 0) event_mqtt_publish = millis();
 
    // *** ПОДГОТОВКА ДАННЫХ
    static char pressure[10];
    dtostrf(pres, 8, 2, pressure);
    static char temperature[10];
    dtostrf(temp, 8, 2, temperature);
    static char humidity[10];
    dtostrf(hum, 8, 2, humidity);
 
    // *** ПУБЛИКАЦИЯ ПО ОДНОМУ СООБЩЕНИЮ ЗА ЦИКЛ   
    switch (step_mqtt_send) {
    case 0:    
      client.publish("/BigGreenHouse/status/pressure", pressure);
      Serial.print("MQTT client publish pressure = ");
      Serial.println(pressure);
      break;
    case 1:    
      client.publish("/BigGreenHouse/status/temperature", temperature);
      Serial.print("MQTT client publish temperature = ");
      Serial.println(temperature);
      break;
    case 2:    
      client.publish("/BigGreenHouse/status/humidity", humidity);
      Serial.print("MQTT client publish humidity = ");
      Serial.println(humidity);
      break;
    case 3:    
      client.publish("/BigGreenHouse/status/watering", String(fan_ctrl()).c_str());
      Serial.print("MQTT client publish fan_ctrl = ");
      Serial.println(fan_ctrl());
      break;
    case 4:    
      //client.publish("/BigGreenHouse/.../...", ...);
      break;
    case 5:    
      //client.publish("/BigGreenHouse/.../...", ...);
      break;
    case 6:    
      //client.publish("/BigGreenHouse/.../...", ...);
      break;        
    }

    step_mqtt_send += 1;
    if ((step_mqtt_send > 5) || (step_mqtt_send >= 255)) {
      mqtt_send = false;
      step_mqtt_send = 0;
    }
  }  
}







//-----------------------------------------------------------------------------
//                ПОДКЛЮЧЕНИЕ К WiFi и MQTT. ПОДПИСКА НА ТОПИКИ 
//----------------------------------------------------------------------------- 
void connect_mqtt() {
  event_reconnect = millis();
  
  // *** ЕСЛИ ОТСУТСТВУЕТ ПОДКЛЮЧЕНИЕ К WiFi РОУТЕРУ - ПОДКЛЮЧИТЬСЯ 
  if (WiFi.status() != 3) {    // WiFi.status() == 3 : WL_CONNECTED
    if (!sta_ok) connect_wifi();        // подключиться к роутеру, если ранее подключение к роутеру отсутствовало 
    else WiFi.mode(WIFI_AP_STA);          // включить точку доступа, если соединение с роутером временно отсутствует
    //else WiFi.softAP(ap_ssid, ap_pass);    
  }

  // *** ЕСЛИ WiFi ЕСТЬ, НО ОТСУТСТВУЕТ ПОДКЛЮЧЕНИЕ К БРОКЕРУ - ПОДКЛЮЧИТЬСЯ 
  if (WiFi.isConnected() && !client.connected()) {
    Serial.print("Attempting MQTT connection...");  // Попытка подключиться к MQTT-брокеру... 
      
    // *** ПОСЛЕ ПОДКЛЮЧЕНИЯ К БРОКЕРУ ПОДПИСАТЬСЯ НА ТОПИК(-И) 
    //     функция client.connect(clientID) выполняет подключение clientID к брокеру
    //     возвращает : false = ошибка подключения / true = соединение выполнено успешно.
    if (client.connect("BGH_aero")) {               
      Serial.println("MQTT connected OK");
      client.setCallback(callback);
      client.subscribe("BigGreenHouse/cmd/watering");  // подписка на топики
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
void connect_wifi() {
 
  // *** STA WiFi - настройки и подключение к роутеру ***
  if (WiFi.getAutoConnect() != true) {    //configuration will be saved into SDK flash area
    WiFi.setAutoConnect(true);            //on power-on automatically connects to last used hwAP
    WiFi.setAutoReconnect(true);          //automatically reconnects to hwAP in case it's disconnected
  }
 
  delay(100);  

  // - Keenetic-2927 - 
  if (WiFi.status() != 3) {
  // if (WiFi.isConnected() != true) {
    ssid = "Keenetic-2927";
    pass = "dUfWKMTh";
    WiFi.config(IPAddress(192,168,123,20), IPAddress(192,168,123,1), IPAddress(255,255,255,0), IPAddress(192,168,123,1));
    mqtt_server = "192.168.123.222";
    mqtt_port = 1883;
    sta_wifi();
 }
    
  // - WiHome -
  if (WiFi.status() != 3) {
  // if (WiFi.isConnected() != true) {
    ssid = "WiHome";
    pass = "Ktcyfz_l8F-50";
    WiFi.config(IPAddress(192,168,50,20), IPAddress(192,168,50,1), IPAddress(255,255,255,0), IPAddress(192,168,50,1));
    //WiFi.config(0, 0, 0, 0);
    mqtt_server = "192.168.50.222";
    mqtt_port = 1883;
    sta_wifi();
  }

  // - ZyXEL01 -
  if (WiFi.status() != 3) {
  // if (WiFi.isConnected() != true) {
    ssid = "ZyXEL01";
    pass = "D3i@GP4%Id";
    WiFi.config(0, 0, 0, 0);
    mqtt_server = "192.168.1.222";
    mqtt_port = 1883;
    sta_wifi();
  }

  if (WiFi.status() == WL_CONNECTED) {
  // if (WiFi.isConnected()) {
    sta_ok = true;                            // подключение к роутеру состоялось
    WiFi.mode(WIFI_STA);                      // переключить в режим = только станция ... или ...
    //WiFi.softAPdisconnect(true);            // отключить точку доступа ESP
    client.setServer(mqtt_server, mqtt_port); // инициализация MQTT брокера

    Serial.print("Connected to ");  // Подключено к 
    Serial.println(WiFi.SSID());
    Serial.print("IP address : ");  // IP-адрес :
    Serial.println(WiFi.localIP());
  }
}

// ===== ПОДКЛЮЧЕНИЕ К WiFi РОУТЕРУ =====
void sta_wifi() {
  Serial.print("To WiFi : ");
  Serial.println(ssid);
  WiFi.begin(ssid.c_str(), pass.c_str()); // инициализация подключения к роутеру
  byte tries = 10;
  while(--tries &&  WiFi.status() != WL_CONNECTED) { 
    delay(1000);
    Serial.print(".");
  }
  Serial.println("");
  Serial.print("WiFi.status = ");
  Serial.println(WiFi.status());
}


//-----------------------------------------------------------------------------
//                          СТАРТОВАЯ ИНИЦИАЛИЗАЦИЯ 
//-----------------------------------------------------------------------------
void setup() {
  // Serial port
  Serial.begin(115200);
  
  // *** GPIO MODE 
  pinMode(fan, OUTPUT);  
  pinMode(ESP_BUILTIN_LED, OUTPUT);  
  
  // *** GPIO STATE 
  //digitalWrite(tmr_rst, HIGH);
  digitalWrite(fan, LOW);
  digitalWrite(ESP_BUILTIN_LED, LOW);

  // *** VARIABLE STATE 
  ctrl = 0; // режим: auto
  rem_fan = false;  // дистанционная команда для реле вентилятора = выключить
  auto_fan = false;  // локальная команда для реле вентилятора = выключить
  man_fan = false;
  
  // *** ИНИЦИАЛИЗАЦИЯ ОПРОСА BME280 ЧЕРЕЗ I2C
  Wire.begin();
  event_bme_begin = millis();
  while(!bme.begin() && millis() - event_bme_begin < 5000); // ожидание подключения BME280
  sensors_read();
        
  // *** ИНИЦИАЛИЗАЦИЯ ТОЧКИ ДОСТУПА (WiFi_AP)
  WiFi.hostname("BGH_aero");     // DHCP Hostname (useful for finding device for static lease)
  WiFi.softAPConfig (ap_ip, ap_gateway, ap_subnet);  
  WiFi.softAP(ap_ssid, ap_pass);
  
  // *** MQTT BROKER. ПОДКЛЮЧЕНИЕ К РОУТЕРУ (WiFi_STA)
  IPAddress dns2(8,8,8,8);
  WiFi.disconnect();
  connect_mqtt();
   
  // *** WEB-СЕРВЕР
  http_server();  
  
  Serial.println("Ready");  //  "Готово" 
  digitalWrite(ESP_BUILTIN_LED, HIGH);
  
  // *** ИНИЦИАЛИЗАЦИЯ ТАЙМЕРОВ СОБЫТИЙ
  mem = now();
  event_serial_output = millis();
  //watering_sta = false;
}

//-----------------------------------------------------------------------------
//                                ОСНОВНОЙ ЦИКЛ
//-----------------------------------------------------------------------------
void loop() {
  // *** режимы управления 
  if (ctrl > 2) ctrl = 2;
  if (ctrl < 0) ctrl = 0;

  if (ctrl == 0) man_fan = auto_fan;
  if (ctrl == 1) man_fan = rem_fan;

  // *** включение реле вентиляции
  //digitalWrite(fan, fan_ctrl());
  digitalWrite(fan, man_fan);
  
  // *** обработка запросов к WEB-серверу
  server.handleClient();

  // *** клиент MQTT
  if(client.connected()) {
    client.loop();
    if ((millis() - event_mqtt_publish) >= 10000) mqtt_send = true;
    publishData(); 
  }
  
  // *** проверка подключения к WiFi и МQTT брокеру и, при необходимости, переподключение
  else if((millis() - event_reconnect) >= 60000) connect_mqtt(); 
     
  // *** опрос датчиков формирование локальных команд
  if((millis() - event_sensors_read) >= 60000) sensors_read();
      
  // *** вывод информации в порт    
  if (millis() - event_serial_output > 20000) { // повторять через 20 секунд
    event_serial_output = millis();
    Serial.print("ctrl = ");  
    Serial.println(ctrl);
    if (!digitalRead(fan)) Serial.println("Реле вентилятора = ВЫКЛ.");  
    if (digitalRead(fan)) Serial.println("Реле вентилятора = ВКЛ."); 
    
    WiFi.printDiag(Serial);
    //Serial.println("SSID : ");
    //Serial.println(String(ssid));
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
    state +=F("Присвоен IP адрес: ");
    state += WiFi.localIP().toString();
    Serial.println(((WiFi.status() == WL_CONNECTED))? state : "Не подключен к роутеру"); 
    Serial.print("MAC: ");
    Serial.println(WiFi.macAddress());
  }

  //delay(1000);
}
