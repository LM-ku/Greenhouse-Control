#define FW_VERSION "FW 10.03.2020"

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

// === WiFi AP Mode === 
IPAddress ap_ip(192,168,123,20);
IPAddress ap_gateway(192,168,123,20);
IPAddress ap_subnet(255,255,255,0);
String ap_ssid = "BigGreenHouse";
String ap_pass = "12345678";

// === WiFi STA Mode & MQTT broker === 
boolean sta_wifi = false;
String ssid, pass;
char* mqtt_server;
int mqtt_port;
String mqtt_user = "";
String mqtt_pass = "";

byte step_mqtt_send = 0; // переменная для поочередной (по одному сообщению за цикл loop) публикации сообщений на MQTT брокере
String id_messages_ = "";
boolean send_ = 0;

WiFiClient espClient;
PubSubClient client(espClient); 


// === WEB SERVER ===
ESP8266WebServer server(80);
//byte len_ssid, len_pass, len_mqtt_server, len_mqtt_port, len_mqtt_user, len_mqtt_pass;
//const char* serverIndex = "<form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='update' /><input type='submit' value='Update' /></form>";
const char* upd_str = "<form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='update' /><input type='submit' value='Start update' /></form>";
const char* root_str = "<!DOCTYPE HTML><html><head><meta http-equiv='refresh' content='1; /'></head></html>\n\r";
const char* ctrl_str = "<!DOCTYPE HTML><html><head><meta http-equiv='refresh' content='1; /ctrl_mode'></head></html>\n\r";

// === BME280 ===
BME280I2C bme;
float temp, pres, hum, moist;
float set_temp = 24.0;  // не ниже 20*С, оптимально 20...25*С
float set_hum = 50.0; // оптимальная влажность воздуха 45...60% 
float set_moist = 65.0; // оптимальная влажность почвы 60...70% или более
bool bme_rdy; // готовность датчика

// === GPIO ===
//const byte tmr_rst = 16;
//const byte tmr_opn = 12;
//const byte tmr_cls = 14;
//const byte valve = 13;
const byte fan = 15;
const byte ESP_BUILTIN_LED = 2;

// === CTRL - управление ===
//byte last_irrig_day, last_irrig_hour, last_irrig_minute;
byte ctrl; // режим: 0 = auto, 1 = remote, 2 = manual
boolean first_scan;
boolean man_fan;
boolean rem_fan;  // дистанционная команда для реле вентилятора: false = выключить, true = включить
boolean auto_fan;  // автоматическая команда для реле вентилятора: false = выключить, true = включить

// === NTP ===
WiFiUDP Udp;
//unsigned int localPort = 2390;  // локальный порт для прослушивания UDP-пакетов
unsigned int localPort = 8888;  // локальный порт для прослушивания UDP-пакетов
//static const char ntpServerName[] = "ntp1.stratum2.ru";
//static const char ntpServerName[] = "ntp2.stratum2.ru";
//static const char ntpServerName[] = "ntp3.stratum2.ru";
//static const char ntpServerName[] = "ntp4.stratum2.ru";
static const char ntpServerName[] = "ntp5.stratum2.ru";
//static const char ntpServerName[] = "ntp1.stratum1.ru";
//static const char ntpServerName[] = "ntp2.stratum1.ru";
//static const char ntpServerName[] = "ntp3.stratum1.ru";
//static const char ntpServerName[] = "ntp4.stratum1.ru"; // не рекомендуется без необходимости !!!
//static const char ntpServerName[] = "ntp5.stratum1.ru"; // не рекомендуется без необходимости !!!
const int timeZone = 5;
const int NTP_PACKET_SIZE = 48;  //  NTP-время – в первых 48 байтах сообщения
byte packetBuffer[NTP_PACKET_SIZE];  //  буфер для хранения входящих и исходящих пакетов 
time_t getNtpTime();

// === ТАЙМЕРЫ СОБЫТИЙ ===
long eventTime; // для снятия текущего времени с начала старта
long event_reconnect;  // для переподключения
long event_sensors_read; // для опроса датчиков
long event_bme_begin; // для подключения  датчика BME280
long event_web_server;
long event_serial_output;
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

// ===== ОПРОС ДАТЧИКА BME280. АВТОМАТИЧЕСКИЕ КОМАНДЫ ВЕНТИЛЯЦИИ ======
void sensors_read(){
  if(((millis() - event_sensors_read) >= 60000) || first_scan) { // выполнять, если после предыдущего опроса датчиков прошло более 1 минуты
    event_sensors_read = millis();
    bme_rdy = bme.begin();

    if (bme_rdy) {
      Serial.println("BME280 подключен");    
      // **** ОПРОС ДАТЧИКА BME280
      BME280::TempUnit tempUnit(BME280::TempUnit_Celsius);
      BME280::PresUnit presUnit(BME280::PresUnit_Pa);
      bme.read(pres, temp, hum, tempUnit, presUnit);  // считать значения давления, температуры, влажности, в метрической системе, давление в Ра 
      //  temp -= 0.3;  // correct <temp>
      pres /= 133.3;  // convert <pres> to mmHg

     
      // *** АВТОМАТИЧЕСКАЯ КОМАНДА УПРАВЛЕНИЯ РЕЛЕ ВЕНТИЛЯТОРА
      if (temp >= set_temp + 1.0) auto_fan = true; // включить вентилятора втоматически, если жарко
      if (temp <= set_temp - 1.0 && hum < set_hum ) auto_fan = false;  // выключить вентилятор автоматически, если не жарко и влажность в норме 
      if (temp <= 20.0) auto_fan = false;  // выключить вентилятор автоматически, если температура не выше 20 град.С
    }
    else {
      pres = 999.99;
      temp = 99.99;
      hum = 99.99;
      Serial.println("BME280 не найден !!!");
      if (ctrl == 0) ctrl = 2;
      //auto_fan = false;
    }

  
    // *** ВЫВОД ДАННЫХ ОТ ДАТЧИКОВ В ПОРТ
    Serial.print ("Температура воздуха   ");
    Serial.print (temp);
    Serial.println ("   *C");
  
    Serial.print ("Влажность воздуха   ");
    Serial.print (hum);
    Serial.println ("   %");
  
    Serial.print ("Давление   ");
    Serial.print (pres);
    Serial.println ("   mmHg");

    Serial.print("Автоматическая команда реле вентилятора = ");  
    Serial.println((auto_fan)? "ВКЛ." : "ВЫКЛ."); 
  }
}


// ===== КОМАНДА УПРАВЛЕНИЯ РЕЛЕ ВЕНТИЛЯТОРА =====
boolean fan_cmd() {
  boolean cmd = ((ctrl==0) && auto_fan) || ((ctrl==1) && rem_fan) || ((ctrl==2) && man_fan);
  return cmd;
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
  return(out);
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
  return(ansver3);   
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
  server.on("/ventilation_set", ventilation_set);
  server.on("/ventilation_ctrl", ventilation_ctrl);

  server.on("/ctrl_mode", ctrl_mode);
  server.on("/ctrl_mode_set", ctrl_mode_set);
     
  server.on("/date_time", date_time);
  server.on("/date_time_set", date_time_set);
  
/*
  server.on("/upd", HTTP_GET, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", upd_str);
  });
*/
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
  //str += F(" max-width: 400px;");
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


// ===== ГЛАВНАЯ СТРАНИЦА. СОСТОЯНИЕ ВЕНТИЛЯЦИИ =====
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
  str += F("<title>ВЕНТИЛЯЦИЯ В ТЕПЛИЦЕ</title>");
  str += F("<style type='text/css'>");
  
  str += F("body {");   
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
  str += F(" width: 200px;");
  str += F("}");
  
  str += F(".ctrl_txt {");
  str += F(" display: inline-block;");
  str += F(" width: 165px;");
  str += F("}");

  str += F(".set_txt {");
  str += F(" display: inline-block;");
  //str += F(" text-align: right;");
  str += F(" width: 115px;");
  str += F("}");

  str += F(".comm_txt {");
  str += F(" display: inline-block;");
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

  str += F(".state_on  {");
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

  str += F(".ctrl_mode{");
  str += F(" line-height: 36px;");
  str += F(" display: inline-block;");
  str += F(" width: 160px;");
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
  str += F(" position:relative;");
  str += F(" top:2px;");
  str += F(" box-shadow: 0 0 0 0 rgba(0,0,0,0.2), 0 0 0 0 rgba(0,0,0,0.19);");
  str += F("}");

  str += F(".set_val {");
  str += F(" padding: 16px;");
  str += F(" width: 100px;");
  str += F(" position: relative;");
  str += F("  text-align: center;"); 
  str += F("  -webkit-transition: all 0.30s ease-in-out;");
  str += F("  -moz-transition: all 0.30s ease-in-out;");
  str += F("  -ms-transition: all 0.30s ease-in-out;");
  str += F("  outline: none;");
  str += F("  box-sizing: border-box;");
  str += F("  -webkit-box-sizing: border-box;");
  str += F("  -moz-box-sizing: border-box;");
  str += F("  background: #fff;");
  str += F("  margin-bottom: 4%;");
  str += F("  border: 1px solid #ccc;");
  str += F("  padding: 3%;");
  str += F("  color: #555;");
  str += F("  font-size: 100%;");
  str += F("}");

  str += F(".set_val:focus {");
  str += F("  box-shadow: 0 0 5px #276873;");
  str += F("  padding: 3%;");
  str += F("  border: 2px solid #276873;");
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
  str += F("<p>Дата ");
  str += F("<span class='data_time' id='date_txt'></span> г.  Время "); 
  str += F("<span class='data_time' id='time_txt'> </span> </p><hr/>");
  
  //str += F("<a href='/date_time'><button class='data_time'>");
  //str += String(day());
  //str += F(".");
  //str += String(month());
  //str += F(".");
  //str += String(year());
  //str += F(" г. </button></a>  Время : ");
  //str += F("<a href='/date_time'><button class='data_time'>");
  //str += String(hour());
  //str += F(":");
  //str += digits_to_String(minute());
  //str += F(":");
  //str += digits_to_String(second());
  //str += F("</button></a></p><hr />");

  //str += F("<hr/><span id='date_txt'></span> &nbsp&nbsp;"); 
  //str += F("<span id='time_txt'></span> <hr/>"); 
   
  // *** Вентиляция 
  str += F("<div class='align_center'><h2>ВЕНТИЛЯЦИЯ В ТЕПЛИЦЕ</h2></div><hr />");
  
  // *** Показания датчиков 
  //if (bme_rdy) {
    str += F("<p><font class='sensors_txt'> Атмосферное давление: </font>");
    str += F("<font class='sensors_value'>");
    str += String(pres);
    str += F(" mm.Hg </font></p>");
    str += F("<p><font class='sensors_txt'>Температура воздуха: </font>");
    str += F("<font class='sensors_value'>");
    str += String(temp);
    str += F(" град.С </font></p>");
    str += F("<p><font class='sensors_txt'>Влажность воздуха: </font>"); 
    str += F("<font class='sensors_value'>");
    str += String(hum);
    str += F(" % </font></p>");
  //}
  //else {
  //  str += F("<p><font class='align_center'> ОШИБКА ИЗМЕРЕНИЯ !!! </font></p>");
  //}
  
  // *** Состояние вентиляции
  str += F("<p><font class='ctrl_txt'> Вентиляция: </font>");
  if(digitalRead(fan))  str += F("<font class='state_on'> ВКЛЮЧЕНА");
  if(!digitalRead(fan)) str += F("<font class='state_off'> ВЫКЛЮЧЕНА");
  str += F("</font></p><hr/>");

  // *** Режим управления
  str += F("<p><font class='ctrl_txt'> Режим управления: </font>");
  str += F("<a href='/ctrl_mode'><button class='");
  if(ctrl == 0) str += F("ctrl_mode'>АВТОМАТИЧЕСКИЙ");
  if(ctrl == 1) str += F("ctrl_mode'>ДИСТАНЦИОННЫЙ");
  if(ctrl == 2) str += F("ctrl_mode'>ВРУЧНУЮ");
  str += F("</button></a></p>"); 

  // *** Кнопки управления ВРУЧНУЮ
  if (ctrl == 2) {
    str += F("<a href='/ventilation_ctrl?cmd=on'><button class='ctrl_on'> ВКЛЮЧИТЬ </button></a>&nbsp");
    str += F("<a href='/ventilation_ctrl?cmd=off'><button class='ctrl_off'> ВЫКЛЮЧИТЬ </button></a></p>");
  }
  str += F("<hr/>");

  // *** Уставки АВТОМАТИЧЕСКОГО режима
  str += F("<div class='align_center'>УСТАВКИ АВТОМАТИЧЕСКОГО РЕЖИМА</div><hr />");
  str += F("<form  method='POST' action='/ventilation_set'>");
  str += F("<p><font class='set_txt'> Tемпература </font>");
  str += F("<input type='TEXT' class='set_val'  name='set_temp' placeholder=' "); 
  str += float_to_String(set_temp);
  str += F("\n'/> +/- 1.0 °С </p>");
  str += F("<p><font class='set_txt'> Влажность </font>");
  str += F("<input type='TEXT' class='set_val' name='set_hum' placeholder=' ");
  str += float_to_String(set_hum);
  str +=F("\n'/>  % </p>");
  str += F("<p><font class='set_txt'> </font><input type='SUBMIT' class='set_button' value='ПРИМЕНИТЬ УСТАВКИ'></p>");
  str += F("</form><hr/>");

  // *** Информация о подключениях
  str += F("<div class='align_center'>ИНФОРМАЦИЯ О ПОДКЛЮЧЕНИЯХ</div><hr />");
  str += F("<p><font class='comm_txt'> WiFi SSID : </font>");
  str += F("<font class='comm_val'>");
  //str += ssid;
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
void ventilation_ctrl() {
  if (server.arg(0)=="on") man_fan = true;  
  if (server.arg(0)=="off") man_fan = false; 
  server.send ( 200, "text/html", root_str );   
  delay(1000);
}

// *** Изменить уставки АВТОМАТИЧЕСКОГО режима ***
void ventilation_set(){
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
  str += F(" width: 165px;");
  str += F("}");

  str += F(".state {");
  str += F(" line-height: 36px;");
  str += F(" display: inline-block;");
  str += F(" width: 160px;");
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
  str += F(" width: 160px;");
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
  str += F("<p><font class='ctrl_txt'> Изменить режим :</font></p>");
  str +=F("<div class='align_center'>");
  str += F("<p><a href='/ctrl_mode_set?ctrl=rem'><button class='ctrl_mode'> ДИСТАНЦИОННЫЙ </button></a></p>");  
  str += F("<p><a href='/ctrl_mode_set?ctrl=auto'><button class='ctrl_mode'> АВТОМАТИЧЕСКИЙ </button></a></p>");
  str += F("<p><a href='/ctrl_mode_set?ctrl=man'><button class='ctrl_mode'> ВРУЧНУЮ </button></a></p>");
  str += F("<hr/><br/>");
  str += F("<p><a href='/'><button class='home_button'> НА ГЛАВНЫЙ ЭКРАН </button></a></p>");
  str += F("</div>");         
  str += F("</body>");
  str += F("</html>\n\r");
  server.send ( 200, "text/html", str );
  delay(1000); 
}

// *** Изменить режим управления ***
void ctrl_mode_set() {
  //Serial.print("ctrl_mode... = ");
  //Serial.println(server.arg(0));
  if (server.arg(0)=="auto") ctrl = 0;
  if (server.arg(0)=="rem")  ctrl = 1;
  if (server.arg(0)=="man") ctrl = 2;
  server.send ( 200, "text/html", root_str );
  delay(1000);
}

// ===== WEB-СЕРВЕР. ИЗМЕНЕНИЕ ДАТЫ И ВРЕМЕНИ =====
void date_time(){
String str = "";
  str += F("<!DOCTYPE HTML>");
  str += F("<html>");
  str += F("<head>");
  str += F("<meta charset='UTF-8' name='viewport' content='width=device-width, initial-scale=1'>");
  str += F("<title>УСТАНОВИТЬ ДАТУ И ВРЕМЯ</title>");
  str += F("<style type='text/css'>");
  
  str += F("body {"); 
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
  str += F(" width: 80px;");
  str += F("}");

  str += F(".data_time {");
  str += F(" padding: 16px;");
  str += F(" width: 50px;");
  str += F(" position: relative;");
  str += F("  text-align: center;"); 
  str += F("  -webkit-transition: all 0.30s ease-in-out;");
  str += F("  -moz-transition: all 0.30s ease-in-out;");
  str += F("  -ms-transition: all 0.30s ease-in-out;");
  str += F("  outline: none;");
  str += F("  box-sizing: border-box;");
  str += F("  -webkit-box-sizing: border-box;");
  str += F("  -moz-box-sizing: border-box;");
  str += F("  background: #fff;");
  str += F("  margin-bottom: 4%;");
  str += F("  border: 1px solid #ccc;");
  str += F("  padding: 3%;");
  str += F("  color: #555;");
  str += F("  font-size: 100%;");
  str += F("}");

  str += F(".data_time:focus {");
  str += F("  box-shadow: 0 0 5px #276873;");
  str += F("  padding: 3%;");
  str += F("  border: 2px solid #276873;");
  str += F("}");

  str += F(".year {");
  str += F(" padding: 16px;");
  str += F(" width: 80px;");
  str += F(" position: relative;");
  str += F("  text-align: center;"); 
  str += F("  -webkit-transition: all 0.30s ease-in-out;");
  str += F("  -moz-transition: all 0.30s ease-in-out;");
  str += F("  -ms-transition: all 0.30s ease-in-out;");
  str += F("  outline: none;");
  str += F("  box-sizing: border-box;");
  str += F("  -webkit-box-sizing: border-box;");
  str += F("  -moz-box-sizing: border-box;");
  str += F("  background: #fff;");
  str += F("  margin-bottom: 4%;");
  str += F("  border: 1px solid #ccc;");
  str += F("  padding: 3%;");
  str += F("  color: #555;");
  str += F("  font-size: 100%;");
  str += F("}");

  str += F(".year:focus {");
  str += F("  box-shadow: 0 0 5px #276873;");
  str += F("  padding: 3%;");
  str += F("  border: 2px solid #276873;");
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
  str += F("</head>");
  str += F("<body>");
  
  str += F("<div class='align_center'><h2>ИЗМЕНЕНИЕ ДАТЫ И ВРЕМЕНИ</h2></div><hr />");
  str += F("<form method='POST' action='/date_time_set'>");

  // *** Дата
  str += F("<p><font class='ctrl_txt'> Дата : </font>");
  str += F("<input type='TEXT' class='data_time' name='day' placeholder='");
  str += String(day());
  str += F("\n'/> / "); 
  str += F("<input type='TEXT' class='data_time' name='month' placeholder='");
  str += String(month());
  str += F("\n'/> / ");
  str += F("<input type='TEXT' class='year' name='year' placeholder='");
  str += String(year());
  str += F("\n'/> г. </p> ");
  
  // *** Время
  str += F("<p><font class='ctrl_txt'> Время : </font>");
  str += F("<input type='TEXT' class='data_time' name='hour' placeholder='");
  str += String(hour());
  str += F("\n'/> час. &nbsp");
  str += F("<input type='TEXT' class='data_time' name='minute' placeholder='");
  str += digits_to_String(minute());
  str += F("\n'/> мин. ");
  str += F("</p><hr />");
  str +=F("<div class='align_center'>");
  str += F("<p><input type='SUBMIT' class='set_button' value='УСТАНОВИТЬ' /></p>");
  str += F("</form>");
  str += F("<p><a href='/' class='home_button'> НА ГЛАВНЫЙ ЭКРАН </a></p>");
  str += F("</div>");         
  str += F("</body>");
  str += F("</html>\n\r");           
  server.send ( 200, "text/html", str );
  delay(1000);   
}

// *** Установить новые ДАТУ и ВРЕМЯ ***
void date_time_set() {
  int day = server.arg(0).toInt();
  int month = server.arg(1).toInt();
  int yr = server.arg(2).toInt();
  int hr = server.arg(3).toInt();
  int min = server.arg(4).toInt();
  int sec = 0;
  setTime(hr,min,sec,day,month,yr);
  server.send ( 200, "text/html", root_str );
  delay(1000);  
}


     
//-----------------------------------------------------------------------------
//                           MQTT CALLBACK
//-----------------------------------------------------------------------------
void callback(char* topic, byte* payload, unsigned int length) {
  for (int i=0;i<length;i++) {
    //Serial.print((char)payload[i]);
  }
  //Serial.println();
} 


//-----------------------------------------------------------------------------
//                        CONNECT WiFi. ЗАПРОС К NTP И MQTT 
//----------------------------------------------------------------------------- 
void connect() {
  // *** выполнять при старте или если после предыдущего запроса прошло более 1 минуты
  if(((millis() - event_reconnect) >= 60000) || first_scan){  
    Serial.print("WiFi.status = ");
    Serial.println(WiFi.status());
    event_reconnect = millis(); 
    if (WiFi.status() == 3) {  // WiFi.status() == 3 : WL_CONNECTED
      WiFi.mode(WIFI_STA); 
      if (timeStatus() == timeNotSet) setSyncProvider(getNtpTime);  // если время не было установлено, запрос к NTP-серверу
      if (client.connect("GreenHouseVentilation")) {  // ********* уточнить и изменить
        Serial.println("MQTT connected OK");
        client.publish("BigGreenHouse/status/test","hello world"); // публикация на брокере
        client.subscribe("BigGreenHouse/cmd/ventilation");  // подписка на топики
      } 
      else {
        Serial.print("MQTT connected failed, rc=");
        Serial.println(client.state());
      }
    }
    else {

      // - Keenetic-2927 - 
      if (WiFi.status() != 3) {
        ssid = "Keenetic-2927";
        pass = "dUfWKMTh";
        WiFi.config(IPAddress(192,168,123,20), IPAddress(192,168,123,1), IPAddress(255,255,255,0), IPAddress(192,168,123,1));
        mqtt_server = "192.168.123.222";
        mqtt_port = 1883;
        wifi_sta();
     }
    
     // - WiHome -
      if (WiFi.status() != 3) {
        ssid = "WiHome";
        pass = "Ktcyfz_l8F-50";
        WiFi.config(IPAddress(192,168,50,20), IPAddress(192,168,50,1), IPAddress(255,255,255,0), IPAddress(192,168,50,1));
        mqtt_server = "192.168.50.222";
        mqtt_port = 1883;
        wifi_sta();
     }

      // - ZyXEL01 -
      if (WiFi.status() != 3) {
        ssid = "ZyXEL01";
        pass = "D3i@GP4%Id";
        //WiFi.config(IPAddress(0,0,0,0), IPAddress(0,0,0,0), IPAddress(0,0,0,0), IPAddress(0,0,0,0));
        WiFi.config(0, 0, 0, 0);
        mqtt_server = "192.168.1.222";
        mqtt_port = 1883;
        wifi_sta();
      }
      
    }
  }
}

void wifi_sta() {
  WiFi.disconnect(true);
  delay(100);
  Serial.print("To WiFi : ");
  Serial.println(ssid);
  WiFi.begin(ssid.c_str(), pass.c_str()); // инициализация подключения к роутеру
  byte tries = 10;
  while(--tries &&  WiFi.status() != 3) { 
    Serial.print(".");
    delay(1000);
  }
  Serial.println("");
  Serial.print("WiFi.status = ");
  Serial.println(WiFi.status());
}



//-----------------------------------------------------------------------------
//                      ТЕКУЩЕЕ ВРЕМЯ ОТ NTP-СЕРВЕРА 
//                        (TimeNTP_ESP8266WiFi.ino)
//-----------------------------------------------------------------------------
time_t getNtpTime() {
  IPAddress ntpServerIP; // IP-адрес NTP-сервера
  while (Udp.parsePacket() > 0) ; // отбраковываем все пакеты, полученные ранее 
  Serial.println("Transmit NTP Request");  //  "Передача NTP-запроса" 
  
  // **** подключение к NTP серверу из списка
  WiFi.hostByName(ntpServerName, ntpServerIP);
  Serial.print(ntpServerName);
  Serial.print(": ");
  Serial.println(ntpServerIP);
  sendNTPpacket(ntpServerIP);
  uint32_t beginWait = millis();
  while (millis() - beginWait < 1500) {
    int size = Udp.parsePacket();
    if (size >= NTP_PACKET_SIZE) {
      //Serial.println("Receive NTP Response");  //  "Получение NTP-ответа"
      Udp.read(packetBuffer, NTP_PACKET_SIZE);  // считываем пакет в буфер 
      unsigned long secsSince1900;
      // конвертируем 4 байта (начиная с позиции 40) в длинное целое число: 
      secsSince1900 =  (unsigned long)packetBuffer[40] << 24;
      secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
      secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
      secsSince1900 |= (unsigned long)packetBuffer[43];
      return secsSince1900 - 2208988800UL + timeZone * SECS_PER_HOUR;
    }
  }
  Serial.println("No NTP Response :-(");  //  "Нет NTP-ответа :(" 
  return 0; // если время получить не удалось, возвращаем «0» 
}
 
// ===== ЗАПРОС К NTP-СЕРВЕРУ =====
void sendNTPpacket(IPAddress &address) {
  // задаем все байты в буфере на «0»: 
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // инициализируем значения для создания NTP-запроса
  // (подробнее о пакетах смотрите по ссылке выше) 
  packetBuffer[0] = 0b11100011;   // LI (от «leap indicator», т.е. «индикатор перехода»), версия, режим работы 
  packetBuffer[1] = 0;     // слой (или тип часов) 
  packetBuffer[2] = 6;     // интервал запросов 
  packetBuffer[3] = 0xEC;  // точность 
  // 8 байтов с нулями, обозначающие базовую задержку и базовую дисперсию: 
  packetBuffer[12] = 49;
  packetBuffer[13] = 0x4E;
  packetBuffer[14] = 49;
  packetBuffer[15] = 52;
  // После заполнения всех указанных полей
  // вы сможете отправлять пакет с запросом о временной метке:      
  Udp.beginPacket(address, 123); // NTP-запросы к порту 123
  Udp.write(packetBuffer, NTP_PACKET_SIZE);
  Udp.endPacket();
}

//-----------------------------------------------------------------------------
//                          СТАРТОВАЯ ИНИЦИАЛИЗАЦИЯ 
//-----------------------------------------------------------------------------
void setup() {
  // Serial port
  Serial.begin(115200);
  
  // *** GPIO MODE ***
  pinMode(fan, OUTPUT);  
  pinMode(ESP_BUILTIN_LED, OUTPUT);  
  
  // *** GPIO STATE ***
  //digitalWrite(tmr_rst, HIGH);
  digitalWrite(fan, LOW);
  digitalWrite(ESP_BUILTIN_LED, LOW);

  // *** VARIABLE STATE ***
  ctrl = 0; // режим: auto
  rem_fan = false;  // дистанционная команда для реле вентилятора = выключить
  auto_fan = false;  // локальная команда для реле вентилятора = выключить
  man_fan = false;
  
  // *** инициализация опроса BME280 через I2C
  Wire.begin();
  event_bme_begin = millis();
  while(!bme.begin() && millis() - event_bme_begin < 5000); // ожидание подключения BME280
  if (!bme.begin()) Serial.println("BME280 не найден !!!");  
      
  // *** STA WiFi - инициализация подключения к роутеру ***
  if (WiFi.getAutoConnect() != true) {    //configuration will be saved into SDK flash area
    WiFi.setAutoConnect(true);   //on power-on automatically connects to last used hwAP
    WiFi.setAutoReconnect(true);    //automatically reconnects to hwAP in case it's disconnected
  }
  WiFi.begin();
  delay(5000);
  
  WiFi.hostname("Ventilation");     // DHCP Hostname (useful for finding device for static lease)
  IPAddress dns2(8,8,8,8); 

   // *** AP WiFi - инициализация точки доступа ***
  WiFi.softAPConfig (ap_ip, ap_gateway, ap_subnet);  // конфигурирование точки доступа
  WiFi.softAP(ap_ssid, ap_pass);
  
  
 
   // *** NTP - конфигурирование синхронизации времени 
  Udp.begin(localPort);
  setSyncProvider(getNtpTime);
  setSyncInterval(86400); // повторная синхронизация с NTP сервером каждые 86400 сек. = 24 час.
  
  // *** MQTT broker
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
  delay(1000);

  // *** подключение к роутеру, запросы к NTP и MQTT
  first_scan = true;
  connect();
  

  
  // *** WEB-сервер
  http_server();  
  
  // *** инициализация таймеров событий
  mem = now();
  event_sensors_read = millis();
  event_serial_output = millis();
  event_reconnect = millis();  
  //irrig_sta = false;
  first_scan = false;
  
  Serial.println("Ready");  //  "Готово" 
  digitalWrite(ESP_BUILTIN_LED, HIGH);
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

  
  // *** проверка подключения к WiFi и МQTT брокеру и, при необходимости, переподключение
  connect();  // запрос к NTP серверу и подключение к MQTT-брокеру
   
  // *** опрос датчиков и формирование локальных команд
  sensors_read();

  // *** включение реле вентилятора 
  digitalWrite(fan, fan_cmd());

  
  // *** обработка запросов к WEB-серверу
  server.handleClient();
      
  // *** вывод информации в порт    
  if (millis() - event_serial_output > 20000) { // повторять через 20 секунд
    event_serial_output = millis();
    Serial.print("ctrl = ");  
    Serial.println(ctrl);
    if (!digitalRead(fan)) Serial.println("Реле вентилятора = ВЫКЛ.");  
    if (digitalRead(fan)) Serial.println("Реле вентилятора = ВКЛ."); 
    String str = "";
    str += F("Дата : ");
    str += String(day());
    str += F(".");
    str += String(month());
    str += F(".");
    str += String(year());
    str += F(" г.  Время : ");
    str += String(hour());
    str += F(":");
    str += digits_to_String(minute());
    str += F(":");
    str += digits_to_String(second());
    Serial.println(str); 
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
    Serial.println(((WiFi.status() == 3))? state : "Не подключен к роутеру"); 
    Serial.print("MAC: ");
    Serial.println(WiFi.macAddress());
  }
  client.loop();  
  delay(1000);
 
}
