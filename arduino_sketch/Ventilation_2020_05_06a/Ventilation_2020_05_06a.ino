#define FW_VERSION "Ventilation_2020-05-06"

/*
  EEPROM MAP :
  addr 0...99     - для проверки состояния eeprom : == ap_ssid, иначе произвольные значения
  addr 110        - память состояния битовых переменных
                    bit 0 : man_cmd
                    bit 1 : rem_cmd
                    bit 2 : auto_cmd
                    bit 3 : 
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
#include <Arduino.h>
#include <U8g2lib.h>

// === Index ===
byte n = 0;

// === WiFi AP Mode === 
IPAddress ap_ip(192,168,123,(20+n));
IPAddress ap_gateway(192,168,123,(20+n));
IPAddress ap_subnet(255,255,255,0);
String ap_ssid = "BGH_ventilation";
String ap_pass = "12345678";

// === WiFi STA Mode === 
IPAddress ip, gw, mask;
String ssid, pass;         
byte wifi_sta;            // 0 = уже подключен к роутеру, 1 = подключаться к сети 1, 2 = подключаться к сети 2, ...

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

// === BME280 ===
BME280I2C bme;
float t, p, h, temp, hum;
int pres, lcd_step;
float set_temp = 24.0;  // не ниже 20*С, оптимально 20...25*С
float set_hum = 50.0; // оптимальная влажность воздуха 45...60% 
boolean bme_sensor_ok;
long bme_n;           // для усреднения выходных значений 

// === LCD Display ===
U8G2_SSD1306_128X32_UNIVISION_1_SW_I2C u8g2(U8G2_R0, /* clock=*/ SCL, /* data=*/ SDA, /* reset=*/ U8X8_PIN_NONE);


// === GPIO ===
const byte fan = 15;
const byte ESP_BUILTIN_LED = 2;

// === CTRL - управление ===
byte mode_ctrl;           // режим: 0 = auto, 1 = remote, 2 = manual
boolean set_ctrl;         // для коррекции команд дистанционного управления
boolean eeprom_ok;        // true == EEPROM содержит актуальные данные
boolean ventilation_run;
boolean man_cmd;
boolean rem_cmd;          // дистанционная команда для реле вентилятора: false == выключить, true == включить
boolean auto_cmd;         // команда для реле вентилятора в автономном режиме: false == выключить, true == включить


// === ТАЙМЕРЫ СОБЫТИЙ ===
long eventTime;           // для снятия текущего времени с начала старта
long event_reconnect;     // для переподключения
long event_auto_ctrl;     // для формирования команды автономного режима
long event_bme_begin;     // для подключения  датчика BME280
long event_serial_output;
long event_mqtt_publish;
long event_lcd_output;
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
//                       ОПРОС ДАТЧИКА BME280
//-----------------------------------------------------------------------------
void sensor_read(){
  bme_sensor_ok = false; 
  if (bme.begin()) {
    bme_sensor_ok = true;
    BME280::TempUnit tempUnit(BME280::TempUnit_Celsius);
    BME280::PresUnit presUnit(BME280::PresUnit_Pa);
    bme.read(p, t, h, tempUnit, presUnit);  // считать значения давления, температуры, влажности, в метрической системе, давление в Ра 
    temp = (temp * bme_n + t - 0.3) / (bme_n + 1);
    hum = (hum * bme_n + h) / (bme_n + 1);
    pres = round((pres * bme_n + p / 133.3) / (bme_n + 1));// convert <pres> to mmHg
    bme_n +=1;
  }
}


//-----------------------------------------------------------------------------
//        КОМАНДЫ ВЕНТИЛЯЦИИ ПО ДАТЧИКУ BME280 В АВТОНОМНОМ РЕЖИМЕ 
//-----------------------------------------------------------------------------
void auto_ctrl(){
  event_auto_ctrl = millis();
  bme_n = 0;
  
  if (temp >= set_temp + 1.0) auto_cmd = true;  // включить вентилятора втоматически, если жарко
  if (hum > set_hum) auto_cmd = true;           // включить вентилятора втоматически, если высокая влажность
  if (temp <= set_temp - 1.0 && hum < set_hum ) auto_cmd = false;  // выключить вентилятор автоматически, если не жарко и влажность в норме 
  if (temp <= 20.0) auto_cmd = false;           // выключить вентилятор автоматически, если температура не выше 20 град.С
  if (!bme_sensor_ok) {
    Serial.println("BME280 не найден !!!");     
    auto_cmd = false;                           // выключить вентилятор автоматически, если датчик не найден
    if (mode_ctrl == 0) mode_ctrl = 1;          // если режим автономный, переключить в дистанционный режим 
  }

  /*
  // *** вывод данных от датчика в порт
  Serial.print ("Температура воздуха   ");
  Serial.print (temp);
  Serial.println ("   *C");
  
  Serial.print ("Влажность воздуха   ");
  Serial.print (hum);
  Serial.println ("   %");
  
  Serial.print ("Давление   ");
  Serial.print (pres);
  Serial.println ("   mmHg");

  Serial.print("Команда автономного режима = ");  
  Serial.println((auto_cmd)? "ВКЛ." : "ВЫКЛ."); 
  */
}

//-----------------------------------------------------------------------------
//                           ВЫВОД ИНФОРМАЦИИ НА LCD
//-----------------------------------------------------------------------------
void lcd_output() {
  event_lcd_output = millis();
  static char to_lcd[10];
  String value;
  
  // - температура
  if (lcd_step == 0) {
    value = String(temp) + "*C";
    value.toCharArray(to_lcd, 10);  
    u8g2.firstPage();
    do {
      u8g2.setFont(u8g2_font_logisoso20_tf);
      u8g2.drawStr(20, 26, to_lcd);
    } while ( u8g2.nextPage() );
  }
  // - влажность
  if (lcd_step == 1) {
    value = String(hum) + " %";
    value.toCharArray(to_lcd, 10);  
    u8g2.firstPage();
    do {
      u8g2.setFont(u8g2_font_logisoso20_tf);
      u8g2.drawStr(20,26,to_lcd);
    } while ( u8g2.nextPage() );  
  }
  // - давление
  if (lcd_step == 2) {
    value = String(pres) + "mm.Hg";
    value.toCharArray(to_lcd, 10); 
    u8g2.firstPage();
    do {
      u8g2.setFont(u8g2_font_logisoso20_tf);
      u8g2.drawStr(3,26,to_lcd);
    } while ( u8g2.nextPage() );  
  }
  lcd_step += 1;
  if (lcd_step > 2) lcd_step = 0;
}

//-----------------------------------------------------------------------------
//                     КОМАНДА УПРАВЛЕНИЯ РЕЛЕ ВЕНТИЛЯЦИИ
//-----------------------------------------------------------------------------
boolean fan_ctrl() {
  boolean out = ((mode_ctrl==0) && auto_cmd) || ((mode_ctrl==1) && rem_cmd) || ((mode_ctrl==2) && man_cmd);
  if(out) ventilation_run = true;
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
  
  // *** Обслуживание главной страницы
  server.on("/", handleRoot); 
  server.on("/ventilation_set", ventilation_set);
  server.on("/ventilation_ctrl", ventilation_ctrl);

  // *** Обслуживание страницы изменения режима управления 
  server.on("/ctrl_mode", ctrl_mode);
  server.on("/ctrl_mode_set", ctrl_mode_set);
     
  // *** Обслуживание страницы обновления версии ПО
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

// ===== ГЛАВНАЯ СТРАНИЦА. СОСТОЯНИЕ ВЕНТИЛЯЦИИ =====
void handleRoot(){
  String str = "";
  str += F("<!DOCTYPE HTML>");
  
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

  str += F(".sensors_error {");
  str += F(" line-height: 36px;");
  str += F(" display: inline-block;");
  str += F(" width: 125px;");
  str += F(" position: relative;");
  str += F(" text-align: center;");
  str += F(" font-size: 100%;");
  str += F(" text-decoration: none;");
  str += F(" color: #FF0000;");
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
  str += F("<body onload='sysTime()'>");

  // *** Дата и время
  str += F("<p>Дата : ");
  str += F("<span id='date_txt'></span> г. &nbsp Время : "); 
  str += F("<span id='time_txt'> </span> </p><hr/>");

  // *** Вентиляция 
  str += F("БОЛЬШАЯ ТЕПЛИЦА. ВЕНТИЛЯЦИЯ <hr />");
  
  // *** Показания датчиков 
    str += F("<p><font class='sensors_txt'> Атмосферное давление: </font>");
    if (bme_sensor_ok) str += F("<font class='sensors_value'>");
    else str += F("<font class='sensors_error'>"); 
    str += String(pres);
    str += F(" mm.Hg </font></p>");
    str += F("<p><font class='sensors_txt'>Температура воздуха: </font>");
    if (bme_sensor_ok) str += F("<font class='sensors_value'>");
    else str += F("<font class='sensors_error'>"); 
    str += String(temp);
    str += F(" °С </font></p>");
    str += F("<p><font class='sensors_txt'>Влажность воздуха: </font>"); 
    if (bme_sensor_ok) str += F("<font class='sensors_value'>");
    else str += F("<font class='sensors_error'>"); 
    str += String(hum);
    str += F(" % </font></p>");
  
  // *** Состояние вентиляции
  str += F("<p><font class='ctrl_txt'> Вентиляция: </font>");
  if(digitalRead(fan))  str += F("<font class='state_on'> ВКЛЮЧЕНА");
  if(!digitalRead(fan)) str += F("<font class='state_off'> ВЫКЛЮЧЕНА");
  str += F("</font></p><hr/>");

  // *** Режим управления
  str += F("<p><font class='ctrl_txt'> Режим управления: </font>");
  str += F("<a href='/ctrl_mode'><button class='");
  if(mode_ctrl == 0) str += F("ctrl_mode'>АВТОНОМНЫЙ");
  if(mode_ctrl == 1) str += F("ctrl_mode'>ДИСТАНЦИОННЫЙ");
  if(mode_ctrl == 2) str += F("ctrl_mode'>ВРУЧНУЮ");
  str += F("</button></a></p>"); 

  // *** Кнопки управления ВРУЧНУЮ
  if (mode_ctrl == 2) {
    str += F("<a href='/ventilation_ctrl?cmd=on'><button class='ctrl_on'> ВКЛЮЧИТЬ </button></a>&nbsp");
    str += F("<a href='/ventilation_ctrl?cmd=off'><button class='ctrl_off'> ВЫКЛЮЧИТЬ </button></a></p>");
  }
  str += F("<hr/>");

  // *** Уставки АВТОНОМНОГО режима
  str += F("УСТАВКИ АВТОНОМНОГО РЕЖИМА<hr />");
  str += F("<form  method='POST' action='/ventilation_set'>");
  str += F("<p><font class='set_txt'> Tемпература </font>");
  str += F("<input type='TEXT' class='set_val'  name='set_temp' placeholder=' "); 
  str += float_to_String(set_temp);
  str += F("\n'/><font class='set_txt'> +/- 1.0 °С </font></p>");
  str += F("<p><font class='set_txt'> Влажность </font>");
  str += F("<input type='TEXT' class='set_val' name='set_hum' placeholder=' ");
  str += float_to_String(set_hum);
  str +=F("\n'/><font class='set_txt'>  % </font></p>");
  str += F("<p><font class='set_txt'> </font><input type='SUBMIT' class='set_button' value='ПРИМЕНИТЬ УСТАВКИ'></p>");
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
  str += F("<p><font class='comm_txt'> Версия ПО: </font>");
   str += F("<input type='SUBMIT' class='set_button'  value='");
  str += String(FW_VERSION);
  str += F("'/></p>"); 
  str += F("</form>");
  

  str += F("</body>");
  server.send ( 200, "text/html", str );
  delay(1000); 
}

// *** Управление вентиляцией ВРУЧНУЮ  ***
void ventilation_ctrl() {
  if (server.arg(0)=="on") man_cmd = true;  
  if (server.arg(0)=="off") man_cmd = false; 
  server.send ( 200, "text/html", root_str );   
  delay(1000);
}

// *** Изменить уставки для АВТОНОМНОГО режима ***
void ventilation_set(){
  float set = String_to_float(server.arg(0));
  if (set != 0.0) set_temp = set;  
  if (set_temp < 21.0)  set_temp = 21.0;
  set = String_to_float(server.arg(1)); 
  if (set != 0.0) set_hum = set;  
  if (set_hum < 40.0)  set_hum = 40.0;
  if (set_hum > 90.0)  set_hum = 90.0;
  server.send ( 200, "text/html", root_str ); 
  auto_ctrl();    // при изменении уставок вновь сформировать команду автономного режима
  delay(1000);
}

// ===== WEB-СЕРВЕР. ВЫБОР РЕЖИМА УПРАВЛЕНИЯ =====
void ctrl_mode(){
  String str = "";
  str += F("<!DOCTYPE HTML>");
  str += F("<meta charset='UTF-8' name='viewport' content='width=device-width, initial-scale=1'>");
  str += F("<title>ВЫБОР РЕЖИМА УПРАВЛЕНИЯ</title>");
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
  str += F("<body>");
  
  str += F("ВЫБОР РЕЖИМА УПРАВЛЕНИЯ<hr />");
  
  str += F("<p><font class='ctrl_txt'> Режим управления: </font><font class='state'> ");
  if(mode_ctrl == 0) str += F("АВТОНОМНЫЙ");
  if(mode_ctrl == 1) str += F("ДИСТАНЦИОННЫЙ");
  if(mode_ctrl == 2) str += F("ВРУЧНУЮ");
  str += F("</font></p><hr/>");
  str += F("<p><font class='ctrl_txt'> Изменить режим :</font><a href='/ctrl_mode_set?ctrl=rem'><button class='ctrl_mode'> ДИСТАНЦИОННЫЙ </button></a></p>");  
  str += F("<p><font class='ctrl_txt'></font><a href='/ctrl_mode_set?ctrl=auto'><button class='ctrl_mode'> АВТОНОМНЫЙ </button></a></p>");
  str += F("<p><font class='ctrl_txt'></font><a href='/ctrl_mode_set?ctrl=man'><button class='ctrl_mode'> ВРУЧНУЮ </button></a></p>");

  str += F("<hr/><br/>");
  str += F("<p><a href='/'><button class='home_button'> НА ГЛАВНЫЙ ЭКРАН </button></a></p>");

  str += F("</body>");
  server.send ( 200, "text/html", str );
  delay(1000); 
}

// *** Изменить режим управления ***
void ctrl_mode_set() {
  if (server.arg(0)=="auto") mode_ctrl = 0;
  //if (server.arg(0)=="rem" && client.connected())  mode_ctrl = 1;
  if (server.arg(0)=="rem")  mode_ctrl = 1;
  if (server.arg(0)=="man") mode_ctrl = 2;
  set_ctrl = true;
  server.send ( 200, "text/html", root_str );
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
  str += F(" width: 325px;");
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
// функция выполняется, когда публикуется сообщение в топик, на который есть подписка
//-----------------------------------------------------------------------------
void callback(char* top, byte* payload, unsigned int length) {

  // *** Сообщение прибыло в топик
  String topic(top);
  String message = "";
  for (int i = 0 ; i < length; i++) {
    message += (char)payload[i];
  }
  
  Serial.print(" - В топик : ");
  Serial.print(topic);
  Serial.print(" получено сообщение :"); 
  Serial.print(message);
  Serial.print(" длиной = ");
  Serial.println(length);

  // *** Команда дистанционного режима управления 
  if(topic == "BigGreenHouse/ventilation/ctrl/cmd") {
    if (message == "1" || message == "true" || message == "start") rem_cmd = true;
    if (message == "0" || message == "false" || message == "stop") rem_cmd = false;
  } 
  
  // *** Изменение режима управления
  if (topic == "BigGreenHouse/ventilation/ctrl/mode") {
    if (message == "1" || message == "rem") mode_ctrl = 1;
    if (message == "0" || message == "auto") mode_ctrl = 0;
  }

  // *** Изменение уставок автономного режима
  if (mode_ctrl == 1) {
    float set = String_to_float(message);
    if (topic == "BigGreenHouse/ventilation/ctrl/set_temperature") {
      if (set != 0.0) set_temp = set;  
      if (set_temp < 21.0)  set_temp = 21.0;
    }
    if (topic == "BigGreenHouse/ventilation/ctrl/set_humidity") {
      if (set != 0.0) set_hum = set;  
      if (set_hum < 40.0)  set_hum = 40.0;
      if (set_hum > 90.0)  set_hum = 90.0;
    }
    auto_ctrl();    // вновь сформировать команду автономного режима
  }
}

//-----------------------------------------------------------------------------
//                           MQTT PUBLISH
// функция публикации сообщений на MQTT брокере по одному сообщению за цикл
//-----------------------------------------------------------------------------

void publishData() {
  static char message[10];  
  if(mqtt_send) {
    if (step_mqtt_send == 0) event_mqtt_publish = millis();
 
    // *** ПУБЛИКАЦИЯ ПО ОДНОМУ СООБЩЕНИЮ ЗА ЦИКЛ   
    switch (step_mqtt_send) {

      // *** Состояние выхода управления реле вентиляции  
      case 0:   
        topic = "BigGreenHouse/ventilation/status/out";
        String(fan_ctrl()).toCharArray(message, 10);
        break;

      // *** Актуальный режим управления       
      case 1:
        topic = "BigGreenHouse/ventilation/status/mode";
        String(mode_ctrl).toCharArray(message, 10);
        break;
       
     // *** Показания датчика атмосферного давления     
     case 2:      
       topic = "BigGreenHouse/ventilation/status/pressure";
        dtostrf(pres, 8, 0, message); 
        break;
 
     // *** Показания датчика температуры     
     case 3:      
       topic = "BigGreenHouse/ventilation/status/temperature";
        dtostrf(temp, 8, 2, message); 
        break;

     // *** Показания датчика влажности воздуха    
     case 4:      
       topic = "BigGreenHouse/ventilation/status/humidity";
        dtostrf(hum, 8, 2, message); 
        break;

      // *** Статус датчика BME       
      case 5:
        topic = "BigGreenHouse/ventilation/status/bme_sensor";
        String(bme_sensor_ok).toCharArray(message, 10);
        break;

      // *** Изменить режим управления на MQTT брокере       
      case 6:
        topic = "BigGreenHouse/ventilation/ctrl/mode";
        String(mode_ctrl).toCharArray(message, 10);
        set_ctrl = false;
        break;
    }
    
    client.publish(topic.c_str(), message);
    
    step_mqtt_send += 1;
    if (step_mqtt_send > 6) {
      mqtt_send = false;
      step_mqtt_send = 0;
    }
  }  
}


//-----------------------------------------------------------------------------
//            РЕЖИМ WiFi_STA. ПОДКЛЮЧЕНИЕ К РОУТЕРУ 
//-----------------------------------------------------------------------------
void sta_wifi() {
  
  // - WiHome -
  if (wifi_sta == 1) {
    ssid = "WiHome";
    pass = "Ktcyfz_l8F-50";
    ip = IPAddress(192,168,50,20);
    gw = IPAddress(192,168,50,1);
    mask = IPAddress(255,255,255,0);
    mqtt_server = "192.168.50.222";
    mqtt_port = 1883;
    connect_wifi();
  }
  
  // - Keenetic-2927 - 
  if (wifi_sta == 2) {
    ssid = "Keenetic-2927";
    pass = "dUfWKMTh";
    ip = IPAddress(192,168,123,20);
    gw = IPAddress(192,168,123,1);
    mask = IPAddress(255,255,255,0);
    mqtt_port = 1883;
    connect_wifi();
 }
}

// ===== ПОДКЛЮЧЕНИЕ К WiFi РОУТЕРУ =====
void connect_wifi() {
  Serial.print("To WiFi : ");
  Serial.println(ssid);
  WiFi.config(ip, gw, mask, gw);
  WiFi.begin(ssid.c_str(), pass.c_str()); // инициализация подключения к роутеру
  byte tries = 10;
  while(--tries &&  WiFi.status() != WL_CONNECTED) { 
    delay(1000);
    Serial.print(".");
  }
  Serial.println("");
  Serial.print("WiFi.status = ");
  Serial.println(WiFi.status());

  if (WiFi.status() == WL_CONNECTED) {
    wifi_sta = 0;
    if (WiFi.localIP() == ip) {
       WiFi.softAPdisconnect(true); // отключить точку доступа ESP
    }
    
    Serial.print("Connected to ");  // Подключено к
    Serial.println(WiFi.SSID());
    Serial.print("IP address : ");  // IP-адрес :
    Serial.println(WiFi.localIP());

    // *** инициализация клиента MQTT
    mqtt_client = ap_ssid;
    client.setServer(mqtt_server, mqtt_port);
    client.setCallback(callback);    
  }
  else if (wifi_sta < 2) wifi_sta += 1;
  else wifi_sta = 1; 
}

//-----------------------------------------------------------------------------
//                          СТАРТОВАЯ ИНИЦИАЛИЗАЦИЯ 
//-----------------------------------------------------------------------------
void setup() {
  
  // *** SERIAL PORT
  Serial.begin(115200);
  Serial.println();

  // *** EEPROM
  EEPROM.begin(512);
  int i;
  unsigned char k;
  char ch[100];
  int len=0;
  while(k != '\0' && len<100) {
    EEPROM.get(0+len, k);
    ch[len] = k;
    len++;
  }
  ch[len]='\0';
  eeprom_ok = (String(ch) == ap_ssid);
  
  Serial.println ("=== EEPROM ===");
  Serial.println (String(ch));
  Serial.println (ap_ssid);
  Serial.println (eeprom_ok);


  // *** GPIO MODE 
  pinMode(fan, OUTPUT);  
  pinMode(ESP_BUILTIN_LED, OUTPUT);  
  
  // *** GPIO STATE 
  digitalWrite(fan, LOW);
  digitalWrite(ESP_BUILTIN_LED, LOW);

  // *** VARIABLE STATE 
  if (eeprom_ok) {
    byte state;
    EEPROM.get(114,mode_ctrl);         // режим управления
    
    EEPROM.get(110,state);
    man_cmd = bitRead(state, 0);  // команда ручного режима для реле вентилятора
    rem_cmd = bitRead(state, 1);  // дистанционная команда для реле вентилятора 
    auto_cmd = bitRead(state, 2); // локальная команда для реле вентилятора
 
    EEPROM.get(120,set_temp);     // уставка температуры для включения вентиляции
    EEPROM.get(124,set_hum);      // уставка влажности для включения вентиляции
  }
  else {
    mode_ctrl = 0;    // режим = автономный
    rem_cmd = false;  // дистанционная команда для реле вентилятора = выключить
    auto_cmd = false; // локальная команда для реле вентилятора = выключить
    man_cmd = false;  // команда ручного режима для реле вентилятора = выключить
  }
  
  // *** ИНИЦИАЛИЗАЦИЯ ОПРОСА BME280 ЧЕРЕЗ I2C
  Wire.begin();
  event_bme_begin = millis();
  while(!bme.begin() && millis() - event_bme_begin < 5000); // ожидание подключения BME280
  bme_n = 0;
  sensor_read();

  // *** ИНИЦИАЛИЗАЦИЯ LCD DISPLAY
  u8g2.begin(); 
  lcd_step = 0;
  lcd_output();
          
  // *** ИНИЦИАЛИЗАЦИЯ ТОЧКИ ДОСТУПА (WiFi_AP)
  WiFi.hostname(ap_ssid);     // DHCP Hostname (useful for finding device for static lease)
  WiFi.softAPConfig (ap_ip, ap_gateway, ap_subnet);  
  WiFi.softAP(ap_ssid, ap_pass);
  if (WiFi.getPersistent() == true) WiFi.persistent(false);
   
  // *** НАСТРОЙКИ ПОДКЛЮЧЕНИЯ К РОУТЕРУ (WiFi_STA)
  WiFi.disconnect();
  IPAddress dns2(8, 8, 8, 8);
  if (WiFi.getAutoConnect() != true) {    // configuration will be saved into SDK flash area
    WiFi.setAutoConnect(true);            // on power-on automatically connects to last used hwAP
    WiFi.setAutoReconnect(true);          // automatically reconnects to hwAP in case it's disconnected
  }
  delay(100);
  wifi_sta = 1;
   
  // *** WEB-СЕРВЕР
  http_server();  

  // *** ИНИЦИАЛИЗАЦИЯ ТАЙМЕРОВ СОБЫТИЙ
  mem = now();
  event_serial_output = millis();
  //event_lcd_output = millis();

  Serial.println("Ready");  //  "Готово" 
  digitalWrite(ESP_BUILTIN_LED, HIGH);
}

//-----------------------------------------------------------------------------
//                                ОСНОВНОЙ ЦИКЛ
//-----------------------------------------------------------------------------
void loop() {
  // *** режимы управления 
  if (mode_ctrl > 2) mode_ctrl = 2;
  if (mode_ctrl < 0) mode_ctrl = 0;

  if (mode_ctrl != 2) man_cmd = fan_ctrl(); // "безударный" переход в ручной режим

  // *** опрос датчика BME280
  sensor_read();
  
  // *** формирование команды автономного режима по температуре и влажности
  if((millis() - event_auto_ctrl) >= 60000) auto_ctrl();

  // *** включение реле вентиляции
  digitalWrite(fan, fan_ctrl());

  // *** вывод информации на LCD
  if((millis() - event_lcd_output) >= 10000) lcd_output();
  
  // *** обработка запросов к WEB-серверу
  server.handleClient();

  // *** если подключение к WiFi роутеру отсутствует, подключить
  if (WiFi.status() != WL_CONNECTED) {
    if (wifi_sta == 0) WiFi.softAP(ap_ssid, ap_pass);   // только включить точку доступа, если соединение с роутером уже было установлено ранее
    else {
      sta_wifi();              // попытка подключения к роутеру, если ранее подключение отсутствовало
     }
  }
  
  // *** если подключен к WiFi роутеру, клиент MQTT 
  if (WiFi.status() == WL_CONNECTED) {
     // если клиент подключен к MQTT, публикация данных каждые 10 секунд по одному сообщению за цикл
    if (client.connected()) {
      client.loop();
      if ((millis() - event_mqtt_publish) >= 10000) mqtt_send = true;
      publishData();
    }
    // иначе, повторять попытку подключения к MQTT брокеру каждые 5 секунд
    else if ((millis() - event_reconnect) >= 5000) {
      if (mode_ctrl == 1) mode_ctrl = 0;          // переключить с дистанционного на автономный режим 
      Serial.print("MQTT connected failed, rc="); // подключение к брокеру отсутствует
      Serial.println(client.state());
      event_reconnect = millis();
      if (client.connect(mqtt_client.c_str()))  {
        Serial.print("client MQTT connected OK. Subscribe : ");
        
        // *** Подписка на топики управления вентиляцией
        topic = "BigGreenHouse/ventilation/ctrl/#";
        //client.subscribe(topic.c_str());
        if (client.subscribe(topic.c_str()))  Serial.println(topic);  // подписка на топики
      }
    }
  }  
  
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
    Serial.print("Реле вентилятора = "); 
    Serial.println((digitalRead(fan)) ? "ВКЛ." : "ВЫКЛ."); 
    
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
    state +=F("Присвоен IP адрес: ");
    state += WiFi.localIP().toString();
    Serial.println((WiFi.status() == WL_CONNECTED) ? state : "Не подключен к роутеру"); 
    Serial.print("MAC: ");
    Serial.println(WiFi.macAddress());
  }

  // *** Обновление значений в EEPROM
  char charBuf[100];
  ap_ssid.toCharArray(charBuf, 100);
  EEPROM.put(0, charBuf);     
  EEPROM.put(114,mode_ctrl);         // режим управления
  byte state = 0;
  bitWrite(state, 0, man_cmd);        // команда ручного режима для реле клапана полива
  bitWrite(state, 1, rem_cmd);        // дистанционная команда для реле клапана полива
  bitWrite(state, 2, auto_cmd);       // локальная команда для реле клапана полива
  EEPROM.put(110, state);
  EEPROM.put(120,set_temp);     // уставка температуры для включения вентиляции
  EEPROM.put(124,set_hum);      // уставка влажности для включения вентиляции
  //boolean result =  EEPROM.commit();
  if (!EEPROM.commit()) Serial.println("ERROR! EEPROM commit failed");
  delay(100);
}
