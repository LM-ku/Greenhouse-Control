#define FW_VERSION "FW 28.07.2019"

/*
  EEPROM MAP :
  addr 0...31     - <ssid> для подключения к сети WiFi в режиме станции
  addr 32...95    - <pass> для подключения к сети WiFi в режиме станции
  
  addr 96         - количество символов <ssid> (<=32)   
  addr 97         - количество символов <pass> (<=64)    
  addr 115        - флаг состояния eeprom : free = 0 || 255, user data = 170 = B#10101010
  addr 116        - память состояния выходов
  addr 120...123  - set_temp (float)
  addr 124...127  - set_hum (float)
  addr 128...132  - set_moist (float)
  addr 140        - месяц окончания предыдущего полива
  addr 141        - день окончанияпредыдущего полива
  addr 142        - час окончания предыдущего полива
  addr 143        - минута окончания предыдущего полива
  addr 400        - количество символов <mqtt_server> 
  addr 401        - количество символов <mqtt_port> 
  addr 402        - количество символов <mqtt_user> 
  addr 403        - количество символов <mqtt_pass>
  addr 404...435  - <mqtt_server> (<=32) 
  addr 436...440  - <mqtt_port> (<=5)
  addr 441...456  - <mqtt_user> (<=16)
  addr 457...     - <mqtt_pass>
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

// === WiFi AP Mode === 
IPAddress ap_IP(192,168,123,22);
IPAddress ap_gateway(192,168,123,22);
IPAddress ap_subnet(255,255,255,0);
String ap_ssid = "BigGreenHouse";
String ap_pass = "12345678";

// === WiFi STA Mode === 
String ssid = "Keenetic-2927";
String pass = "dUfWKMTh";

//String ssid = "WiHome";
//String pass = "Ktcyfz_l8F-50";

// === WiFi Scan ===
String wifi_scan = "";
String ssid_set = "";

// === MQTT broker ===
//IPAddress mqtt_server(192, 168, 123, 222);
char* mqtt_server = "192.168.123.222";
int mqtt_port = 1883;
String mqtt_user = "";
String mqtt_pass = "";


byte step_mqtt_send = 0; // переменная для поочередной (по одному сообщению за цикл loop) публикации сообщений на MQTT брокере
String id_messages_ = "";
boolean send_ = 0;

WiFiClient espClient;
PubSubClient client(espClient); 


// === WEB SERVER ===
ESP8266WebServer server(80);
byte len_ssid, len_pass, len_mqtt_server, len_mqtt_port, len_mqtt_user, len_mqtt_pass;
const char* serverIndex = "<form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='update' /><input type='submit' value='Update' /></form>";
const char* root_str = "<!DOCTYPE HTML><html><head><meta http-equiv='refresh' content='1; /'></head></html>\n\r";
const char* ctrl_str = "<!DOCTYPE HTML><html><head><meta http-equiv='refresh' content='1; /ctrl_mode'></head></html>\n\r";
const char* ventilation_str = "<!DOCTYPE HTML><html><head><meta http-equiv='refresh' content='1; /ventilation_adj'></head></html>\n\r";
const char* irrigation_str = "<!DOCTYPE HTML><html><head><meta http-equiv='refresh'  content='1; /irrigation_adj'></head></html>\n\r";

// === BME280 ===
BME280I2C bme;
float temp, pres, hum, moist;
float set_temp = 24.0;  // не ниже 20*С, оптимально 20...25*С
float set_hum = 50.0; // оптимальная влажность воздуха 45...60% 
float set_moist = 65.0; // оптимальная влажность почвы 60...70% или более
int ct_rd; // счетчик измерений
int ct_err; // счетчик ошибок

// === Capacitive Soil Moisture Sensor ===
int analog_sensor;
int air_val = 520;  // показания датчика на воздухе, соответствуют 0%
int water_val = 260;  // показания датчика в воде, соответствуют 100%


// === GPIO ===
const byte tmr_rst = 16;
const byte tmr_opn = 12;
const byte tmr_cls = 14;
const byte valve = 13;
const byte fan = 15;
const byte ESP_BUILTIN_LED = 2;

// === CTRL - управление ===
byte last_irrig_day, last_irrig_hour, last_irrig_minute;
byte ctrl; // режим: 0 = auto, 1 = remote, 2 = manual
boolean first_scan;
boolean irrig_sta;  // состоялся полив после перезагрузки 
boolean man_ctrl;
boolean man_fan;
boolean man_valve;
boolean irrig_state;
boolean rem_valve;  // дистанционная команда для реле клапана полива: false = выключить, true = включить
boolean rem_fan;  // дистанционная команда для реле вентилятора: false = выключить, true = включить
boolean auto_fan;  // автоматическая команда для реле вентилятора: false = выключить, true = включить
volatile boolean auto_valve = false;  // автоматическая команда для реле клапана полива: false = выключить, true = включить
time_t start_irrig; // старт полива
time_t end_irrig; // окончание полива
time_t irrig_cont;  // продолжительность полива
time_t after_irrig;  // прошло времени после окончания полива



// === NTP ===
WiFiUDP Udp;
unsigned int localPort = 2390;  // локальный порт для прослушивания UDP-пакетов
//static const char ntpServerName[] = "ntp1.stratum2.ru";
//static const char ntpServerName[] = "ntp2.stratum2.ru";
//static const char ntpServerName[] = "ntp3.stratum2.ru";
//static const char ntpServerName[] = "ntp4.stratum2.ru";
static const char ntpServerName[] = "ntp5.stratum2.ru";
//static const char ntpServerName[] = "ntp1.stratum1.ru";
//static const char ntpServerName[] = "ntp2.stratum1.ru";
//static const char ntpServerName[] = "ntp3.stratum1.ru";
//static const char ntpServerName[] = "ntp4.stratum1.ru";
//static const char ntpServerName[] = "ntp5.stratum1.ru";
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

//-----------------------------------------------------------------------------
//         ОПРОС ДАТЧИКА BME280. АВТОМАТИЧЕСКИЕ КОМАНДЫ ВЕНТИЛЯЦИИ
//-----------------------------------------------------------------------------
void sensors_read(){
  if(((millis() - event_sensors_read) >= 60000) || first_scan) { // выполнять, если с предыдущего опроса датчиков прошло более 1 минуты
    event_sensors_read = millis();

    if (bme.begin()) {
      Serial.println("BME280 подключен");    
      // **** ОПРОС ДАТЧИКА BME280
      BME280::TempUnit tempUnit(BME280::TempUnit_Celsius);
      BME280::PresUnit presUnit(BME280::PresUnit_Pa);
      bme.read(pres, temp, hum, tempUnit, presUnit);  // считать значения давления, температуры, влажности, в метрической системе, давление в Ра 
      //  temp -= 0.3;  // correct <temp>
      pres /= 133.3;  // convert <pres> to mmHg

     
      // *** АВТОМАТИЧЕСКАЯ КОМАНДА УПРАВЛЕНИЯ РЕЛЕ ВЕНТИЛЯТОРА
      if (temp >= set_temp + 1.0) auto_fan = true; // включить вентилятор, если жарко
      if (temp <= set_temp - 1.0 && hum < set_hum ) auto_fan = false;  // выключить вентилятор, если не жарко и влажность в норме 
      if (temp <= 20.0) auto_fan = false;  // всегда выключить вентилятор, если температура не выше 20 град.С
    }
    else {
        pres = 999.99;
        temp = 99.99;
        hum = 99.99;
        Serial.println("BME280 не найден !!! -> Локальная команда 'Выключить вентиляцию'");
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


//-----------------------------------------------------------------------------
//                      КОМАНДА ДЛЯ УПРАВЛЕНИЕ РЕЛЕ ВЕНТИЛЯТОРА
//-----------------------------------------------------------------------------
boolean fan_cmd() {
  boolean cmd = ((ctrl==0) && auto_fan) || ((ctrl==1) && rem_fan) || ((ctrl==2) && man_fan);
  return cmd;
}


//-----------------------------------------------------------------------------
//                       КОМАНДА ДЛЯ УПРАВЛЕНИЯ РЕЛЕ КЛАПАНА
//-----------------------------------------------------------------------------
boolean valve_cmd() {
  boolean cmd = ((ctrl==0) && auto_valve) || ((ctrl==1) && rem_valve) || ((ctrl==2) && man_valve);
  if(cmd) irrig_sta = true;
  return cmd;
}


//-----------------------------------------------------------------------------
//                      ПРЕОБРАЗОВАНИE IPaddres В STRING (не используется)
//-----------------------------------------------------------------------------
String IpAddress2String(IPAddress & ip){
  String s = String(ip[0]);
  for (int i = 0; i < 4; i++) {
    s += String(".");
    s += String(ip[i]);
  }
  return s;
}

//-----------------------------------------------------------------------------
//            ПРЕОБРАЗОВАНИE МИНУТ И СЕКУНД ИЗ INT В STRING ВИДА "ХХ"
//-----------------------------------------------------------------------------
String digits_to_String(int digits) {
  if (digits < 10) return "0" + String(digits);
  return String(digits);
}


//-----------------------------------------------------------------------------
//                  ПРЕОБРАЗОВАНИE RSSI dBm В ПРОЦЕНТЫ
//-----------------------------------------------------------------------------
unsigned int toWiFiQuality(int32_t rssi) {
  unsigned int  qu;
  if (rssi <= -100)
    qu = 0;
  else if (rssi >= -50)
    qu = 100;
  else
    qu = 2 * (rssi + 100);
  return qu;
}

//-----------------------------------------------------------------------------
//                       ПРЕОБРАЗОВАНИЕ STRING В FLOAT 
//-----------------------------------------------------------------------------
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

//-----------------------------------------------------------------------------
//                       ПРЕОБРАЗОВАНИЕ FLOAT В STRING 
//-----------------------------------------------------------------------------
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
//                     ЗАПИСЬ FLOAT ЗНАЧЕНИЯ В EEPROM
//-----------------------------------------------------------------------------
void EEPROM_float_write(int addr, float val)  { // запись Float в ЕЕПРОМ
  byte *x = (byte *)&val;
  for(byte i = 0; i < 4; i++) EEPROM.put(i+addr, x[i]);
}


//-----------------------------------------------------------------------------
//                    ЧТЕНИЕ FLOAT ЗНАЧЕНИЯ ИЗ EEPROM
//-----------------------------------------------------------------------------
float EEPROM_float_read(int addr) { // чтение Float из ЕЕПРОМ
  byte x[4];
  for(byte i = 0; i < 4; i++) x[i] = EEPROM.read(i+addr);
  float *y = (float *)&x;
  return y[0];
}




//-----------------------------------------------------------------------------  
//                 WEB-СЕРВЕР. ГЛАВНАЯ СТРАНИЦА. СОСТОЯНИЕ ТЕПЛИЦЫ
//-----------------------------------------------------------------------------
void handleRoot(){
  String str = "";
  str += F("<!DOCTYPE HTML>");
  str += F("<html>");
  str += F("<head>");
  str += F("<meta charset='UTF-8' name='viewport' content='width=device-width, initial-scale=1'>");
  str += F("<meta http-equiv='Refresh' content='30' />");
  str += F("<title>СОСТОЯНИЕ ТЕПЛИЦЫ</title>");
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
  
  str += F(".ctrl_txt {");
  str += F(" display: inline-block;");
  str += F(" width: 165px;");
  str += F("}");
    
  str += F(".ctrl {");
  str += F(" line-height: 36px;");
  str += F(" display: inline-block;");
  str += F(" width: 160px;");
  str += F(" position: relative;");
  str += F(" text-align: center;");
  str += F(" text-decoration: none;");
  str += F(" color: #FFF;");
  str += F(" background-color: #006778;");
  str += F(" background-image: -webkit-linear-gradient(top, #0087b0, #006778);");
  str += F(" background-image: linear-gradient(to bottom, #0087b0, #006778);");
  str += F(" border-bottom: solid 3px #004462;");
  str += F(" border-radius: 4px;");
  str += F(" box-shadow: inset 0 1px 0 rgba(255,255,255,0.2), 0 3px 2px rgba(0, 0, 0, 0.19);");
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

  str += F(".ctrl_auto{");
  str += F(" width: 160px;");
  str += F(" background-color: #006778;");
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

  str += F(".ctrl_auto:active {");
  str += F(" position:relative;");
  str += F(" top:2px;");
  str += F(" box-shadow: 0 0 0 0 rgba(0,0,0,0.2), 0 0 0 0 rgba(0,0,0,0.19);");
  str += F("}");

  str += F(".sensors_txt {");
  str += F(" display: inline-block;");
  str += F(" width: 200px;");
  str += F("}");
    
  str += F(".sensors_val {");
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
  
  str += F(".comm_txt {");
  str += F(" display: inline-block;");
  str += F(" width: 110px;");
  str += F("}");
  
  str += F(".comm {");
  str += F(" width: 210px;");
  str += F(" background-color: #006778;");
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
  
  str += F(".comm:active {");
  str += F(" position:relative;");
  str += F(" top:2px;");
  str += F(" box-shadow: 0 0 0 0 rgba(0,0,0,0.2), 0 0 0 0 rgba(0,0,0,0.19);");
  str += F("}");


  str += F("</style>");
  str += F("</head>");
  str += F("<body>");

  // *** Дата и время
  str += F("<p>Дата : ");
  str += F("<a href='/date_time'><button class='data_time'>");
  str += String(day());
  str += F(".");
  str += String(month());
  str += F(".");
  str += String(year());
  str += F(" г. </button></a>  Время : ");
  str += F("<a href='/date_time'><button class='data_time'>");
  str += String(hour());
  str += F(":");
  str += digits_to_String(minute());
  str += F(":");
  str += digits_to_String(second());
  str += F("</button></a></p><hr />");
   
  // *** Режим управления
  str += F("<p><font class='ctrl_txt'> Режим управления: </font>");
  str += F("<a href='/ctrl_mode'><button class='");
  if(ctrl == 0) str += F("ctrl_auto'>АВТОМАТИЧЕСКИЙ");
  if(ctrl == 1) str += F("ctrl_on'>ДИСТАНЦИОННЫЙ");
  if(ctrl == 2) str += F("ctrl_off'>ВРУЧНУЮ");
  str += F("</button></a></p><hr />"); 

 
  // *** Вентиляция 
  str += F("<p><font class='sensors_txt'> Атмосферное давление: </font>");
  str += F("<font class='sensors_val'>");
  str += String(pres);
  str += F(" mm.Hg </font></p>");
  str += F("<p><font class='sensors_txt'>Температура воздуха: </font>");
  str += F("<font class='sensors_val'>");
  str += String(temp);
  str += F(" град.С </font></p>");
  str += F("<p><font class='sensors_txt'>Влажность воздуха: </font>"); 
  str += F("<font class='sensors_val'>");
  str += String(hum);
  str += F(" % </font></p>");   
  str += F("<p><font class='ctrl_txt'> Вентиляция: </font>");
  str += F("<a href='/ventilation_adj'><button class='");
  if(fan_cmd())  str += F("ctrl_on'>ВКЛЮЧЕНА");
  if(!fan_cmd()) str += F("ctrl_off'>ВЫКЛЮЧЕНА");
  str += F("</button></a></p><hr />"); 

  // *** Орошение 
  /*
  str += F("<p><font class='sensors_txt'>Влажность почвы: </font>"); 
  str += F("<font class='sensors_val'>");
  str += String(moist);
  str += F(" % </font></p>");
  */

   after_irrig = now() - end_irrig; // прошло времени после окончания полива 
   str += F("<p>После окончания предыдущего полива </p>");
   str += F("<p><font class='ctrl_txt'> прошло: </font><font class='ctrl' >");
   str += String(day(after_irrig)-1);
   str += F(" дн.");
   str += String(hour(after_irrig));
   str += F("  час.");  
   str += String(minute(after_irrig));
   str += F("  мин.</font></p>");
 
  // *** клапан ОТКРЫТ
  if(valve_cmd()) {  
    irrig_cont = now() - start_irrig; // определение продолжительности  полива
    str += F("<p><font class='ctrl_txt'> От начала полива : </font><font class='ctrl'>");
    if (day(irrig_cont) > 1){
      str += String(day(irrig_cont)-1);
      str += F(" дн.");
    }
    str += String(hour(irrig_cont));
    str += F(" час.");
    str += digits_to_String(minute(irrig_cont));
    str += F(" мин.</font></p>");
    str += F("<p><font class='ctrl_txt'> Клапан полива: </font>");
    str += F("<a href='/irrigation_adj'><button class='ctrl_on'>ОТКРЫТ");
  }

  // *** клапан ЗАКРЫТ
  if(!valve_cmd()) {  
    str += F("<p><font class='ctrl_txt'> Продолжительность : </font><font class='ctrl'>");
    if (day(irrig_cont) > 1){
      str += String(day(irrig_cont)-1);
      str += F(" дн.");
    }
    str += String(hour(irrig_cont));
    str += F(" час.");
    str += digits_to_String(minute(irrig_cont));
    str += F(" мин.</font></p>");
    str += F("<p><font class='ctrl_txt'> Клапан полива: </font>");
    str += F("<a href='/irrigation_adj'><button class='ctrl_off'>ЗАКРЫТ");
  }  
  str += F("</button></a></p><hr />"); 

  // *** Информация о подключениях
  str += F("<p><font class='comm_txt'> WiFi SSID : </font>");
  str += F("<a href='/wifi_setup'><button class='comm'>");
  str += String(ssid);
  str += F("</button></a></p>"); 
  if(WiFi.status() == WL_CONNECTED) {
    str += F("<p><font class='comm_txt'> IP addres: </font>");
    str += F("<font class='comm'>"); 
    str += WiFi.localIP().toString();
    str += F("</font></p>");
    str += F("<p><font class='comm_txt'> WiFi RSSI : </font>");
    if (toWiFiQuality(WiFi.RSSI()) < 30) str += F("<font color = '#FF0000'> ");
    else str += F("<font color = '#009900'> ");
    str += String(toWiFiQuality(WiFi.RSSI()));
    str += F(" % </font></p><hr />");
   
    str += F("<p><font class='comm_txt'> MQTT BROKER: </font>");
    str += F("<a href='/mqtt_update'><button class='comm'>");
    str += String(mqtt_server);
    str += F(" : ");
    str += String(mqtt_port);
    str += F("</button></a></p>"); 
    
    str += F("<p><font class='comm_txt'> To MQTT </font>");
    if(client.connect("BigGreenHouse")) str += F("<font color = '#009900'> CONNECTED !</font></p>");
    else str += F("<font color = '#FF0000'> NOT CONNECTED !!!</font></p>");
  }
  else {
    str += F("<p><font class='comm_txt'> WiFi </font>");
    str += F("<font color = '#FF0000'> NOT CONNECTED !!! </font></p>");
  }
  str += F("<hr />");     
 
  // *** Версия ПО и обновление 
  str += F("<form method='GET' action='upd'>");
  str += F("<p><font class='ctrl_txt'> Текущая версия ПО: </font>");
  str += F("<input type='SUBMIT' class='ctrl_auto'  value='");
  str += String(FW_VERSION);
  str += F("'/></p>"); 
  str += F("</form>");
  
  str += F("</body>");
  //str += F("</html>\n\r");
  str += F("</html>");
  server.send ( 200, "text/html", str );
  delay(1000); 
}


//-----------------------------------------------------------------------------  
//                 WEB-СЕРВЕР. УСТАНОВКА ДАТЫ И ВРЕМЕНИ
//-----------------------------------------------------------------------------
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

  str += F(".ctrl_txt {");
  str += F(" display: inline-block;");
  str += F(" width: 80px;");
  str += F("}");

  str += F(".set_button {");
  //str += F(" background-color: #e4685d;");
  str += F(" background-color: #006778;");
  str += F(" border: none;");
  str += F(" box-shadow: 0 1px 8px 0 rgba(0,0,0,0.2), 0 6px 20px 0 rgba(0,0,0,0.19);");
  str += F(" -moz-border-radius:4px;");
  str += F(" -webkit-border-radius:4px;");
  str += F(" border-radius:4px;");
  str += F(" color: white;");
  str += F(" padding: 10px 30px;");
  str += F(" text-decoration: none;");
  str += F(" display: inline-block;");
  str += F(" font-size: 120%;");
  str += F(" margin-top:24px;");
  str += F("}");
  
  str += F(".set_button:active {");
  str += F(" position:relative;");
  str += F(" top:2px;");
  str += F(" box-shadow: 0 0 0 0 rgba(0,0,0,0.2), 0 0 0 0 rgba(0,0,0,0.19);");
  str += F("}");
  
  str += F(".home_button {");
  str += F(" background-color: #464e7a;");
  str += F(" border: none;");
  str += F(" box-shadow: 0 1px 8px 0 rgba(0,0,0,0.2), 0 6px 20px 0 rgba(0,0,0,0.19);");
  str += F(" -moz-border-radius:4px;");
  str += F(" -webkit-border-radius:4px;");
  str += F(" border-radius:4px;");
  str += F(" color: white;");
  str += F(" padding: 10px 30px;");
  str += F(" text-decoration: none;");
  str += F(" display: inline-block;");
  str += F(" font-size: 120%;");
  str += F(" margin-top:24px;");
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

// *** Установить новые ДАТУ и ВРЕМЯ
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
//                 WEB-СЕРВЕР. ВЫБОР РЕЖИМА УПРАВЛЕНИЯ
//-----------------------------------------------------------------------------
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

  str += F(".ctrl_auto{");
  str += F(" width: 160px;");
  str += F(" background-color: #006778;");
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

  str += F(".ctrl_auto:active {");
  str += F(" position:relative;");
  str += F(" top:2px;");
  str += F(" box-shadow: 0 0 0 0 rgba(0,0,0,0.2), 0 0 0 0 rgba(0,0,0,0.19);");
  str += F("}");

  str += F(".sensors_txt {");
  str += F(" display: inline-block;");
  str += F(" width: 200px;");
  str += F("}");

  str += F(".home_button {");
  str += F(" background-color: #464e7a;");
  str += F(" border: none;");
  str += F(" box-shadow: 0 1px 8px 0 rgba(0,0,0,0.2), 0 6px 20px 0 rgba(0,0,0,0.19);");
  str += F(" -moz-border-radius:4px;");
  str += F(" -webkit-border-radius:4px;");
  str += F(" border-radius:4px;");
  str += F(" color: white;");
  str += F(" padding: 10px 30px;");
  str += F(" text-decoration: none;");
  str += F(" display: inline-block;");
  str += F(" font-size: 120%;");
  str += F(" margin-top:24px;");
  str += F("}");
      
  str += F("</style>");
  str += F("</head>");
  str += F("<body>");
  str += F("<p><font class='ctrl_txt'> Режим управления: </font>");
  if(ctrl == 0) str += F("<font class='ctrl_auto'> АВТОМАТИЧЕСКИЙ");
  if(ctrl == 1)  str += F("<font class='ctrl_on'> ДИСТАНЦИОННЫЙ");
  if(ctrl == 2)  str += F("<font class='ctrl_off'> ВРУЧНУЮ");
  str += F("</font></p><hr/>");
  str += F("<p><font class='ctrl_txt'> Изменить режим :</font></p>");
  str +=F("<div class='align_center'>");
  str += F("<p><a href='/ctrl_mode_set?ctrl=rem'><button class='ctrl_on'> ДИСТАНЦИОННЫЙ </button></a></p>");  
  str += F("<p><a href='/ctrl_mode_set?ctrl=auto'><button class='ctrl_auto'> АВТОМАТИЧЕСКИЙ </button></a></p>");
  str += F("<p><a href='/ctrl_mode_set?ctrl=man'><button class='ctrl_off'> ВРУЧНУЮ </button></a></p>");
  str += F("<hr/><br/>");
  str += F("<p><a href='/'><button class='home_button'> НА ГЛАВНЫЙ ЭКРАН </button></a></p>");
  str += F("</div>");         
  str += F("</body>");
  str += F("</html>\n\r");
  server.send ( 200, "text/html", str );
  delay(1000); 
}

// *** Установить режим управления: АВТОМАТИЧЕСКИЙ, ДИСТАНЦИОННЫЙ или ВРУЧНУЮ
void ctrl_mode_set() {
  delay(1000);
  Serial.print("ctrl_mode... = ");
  Serial.println(server.arg(0));
  if (server.arg(0)=="auto") ctrl = 0;
  if (server.arg(0)=="rem")  ctrl = 1;
  if (server.arg(0)=="man") ctrl = 2;
  server.send ( 200, "text/html", root_str );
  delay(1000);
}


//-----------------------------------------------------------------------------  
//             WEB-СЕРВЕР. НАСТРОЙКИ УПРАВЛЕНИЯ ВЕНТИЛЯЦИЕЙ
//-----------------------------------------------------------------------------
void ventilation_adj(){
  String str = "";
  str += F("<!DOCTYPE HTML>");
  str += F("<html>");
  str += F("<head>");
  str += F("<meta charset='UTF-8' name='viewport' content='width=device-width, initial-scale=1'>");
  str += F("<title>УПРАВЛЕНИЕ ВЕНТИЛЯЦИЕЙ</title>");
  str += F("<style type='text/css'>");
  
  str += F("body {"); 
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
  str += F(" width: 165px;");
  str += F("}");
    
  str += F(".ctrl {");
  str += F(" line-height: 36px;");
  str += F(" display: inline-block;");
  str += F(" width: 200px;");
  str += F(" position: relative;");
  str += F(" text-align: center;");
  str += F(" text-decoration: none;");
  str += F(" color: #FFF;");
  str += F(" background-color: #006778;");
  str += F(" background-image: -webkit-linear-gradient(top, #0087b0, #006778);");
  str += F(" background-image: linear-gradient(to bottom, #0087b0, #006778);");
  str += F(" border-bottom: solid 3px #004462;");
  str += F(" border-radius: 4px;");
  str += F(" box-shadow: inset 0 1px 0 rgba(255,255,255,0.2), 0 3px 2px rgba(0, 0, 0, 0.19);");
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

  str += F(".ctrl_auto{");
  str += F(" width: 160px;");
  str += F(" background-color: #006778;");
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

  str += F(".ctrl_auto:active {");
  str += F(" position:relative;");
  str += F(" top:2px;");
  str += F(" box-shadow: 0 0 0 0 rgba(0,0,0,0.2), 0 0 0 0 rgba(0,0,0,0.19);");
  str += F("}");

  str += F(".sensors_txt {");
  str += F(" display: inline-block;");
  str += F(" width: 200px;");
  str += F("}");
    
  str += F(".sensors_val {");
  str += F(" line-height: 36px;");
  str += F(" display: inline-block;");
  str += F(" width: 125px;");
  str += F(" position: relative;");
  str += F(" text-align: center;");
  str += F(" text-decoration: none;");
  str += F(" color: #FFF;");
  str += F(" background-color: #006778;");
  str += F(" background-image: -webkit-linear-gradient(top, #0087b0, #006778);");
  str += F(" background-image: linear-gradient(to bottom, #0087b0, #006778);");
  str += F(" border-bottom: solid 3px #004462;");
  str += F(" border-radius: 4px;");
  str += F(" box-shadow: inset 0 1px 0 rgba(255,255,255,0.2), 0 3px 2px rgba(0, 0, 0, 0.19);");
  str += F("}");

  str += F(".setting_txt {");
  str += F(" display: inline-block;");
  str += F(" width: 120px;");
  str += F("}");
 
  str += F(".setting_val_{");
  str += F(" line-height: 36px;");
  str += F(" background-color: #00cccc;");
  str += F(" display: inline-block;");
  str += F(" width: 80px;");
  str += F(" position: relative;");
  str += F(" text-align: center;");
  str += F(" color: #000000;");
  str += F(" border-radius: 4px;");
  str += F(" font-size: 12pt;"); 
  str += F("}");
  
  str += F(".setting_val {");
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

  str += F(".setting_val:focus {");
  str += F("  box-shadow: 0 0 5px #276873;");
  str += F("  padding: 3%;");
  str += F("  border: 2px solid #276873;");
  str += F("}");

  str += F(".set_button {");
  //str += F(" background-color: #e4685d;");
  str += F(" background-color: #006778;");
  str += F(" border: none;");
  str += F(" box-shadow: 0 1px 8px 0 rgba(0,0,0,0.2), 0 6px 20px 0 rgba(0,0,0,0.19);");
  str += F(" -moz-border-radius:4px;");
  str += F(" -webkit-border-radius:4px;");
  str += F(" border-radius:4px;");
  str += F(" color: white;");
  str += F(" padding: 10px 30px;");
  str += F(" text-decoration: none;");
  str += F(" display: inline-block;");
  str += F(" font-size: 120%;");
  str += F(" margin-top:24px;");
  str += F("}");
  
  str += F(".set_button:active {");
  str += F(" position:relative;");
  str += F(" top:2px;");
  str += F(" box-shadow: 0 0 0 0 rgba(0,0,0,0.2), 0 0 0 0 rgba(0,0,0,0.19);");
  str += F("}");

  str += F(".home_button {");
  str += F(" background-color: #464e7a;");
  str += F(" border: none;");
  str += F(" box-shadow: 0 1px 8px 0 rgba(0,0,0,0.2), 0 6px 20px 0 rgba(0,0,0,0.19);");
  str += F(" -moz-border-radius:4px;");
  str += F(" -webkit-border-radius:4px;");
  str += F(" border-radius:4px;");
  str += F(" color: white;");
  str += F(" padding: 10px 30px;");
  str += F(" text-decoration: none;");
  str += F(" display: inline-block;");
  str += F(" font-size: 120%;");
  str += F(" margin-top:24px;");
  str += F("}");
  
  str += F(".home_button:active {");
  str += F(" position:relative;");
  str += F(" top:2px;");
  str += F(" box-shadow: 0 0 0 0 rgba(0,0,0,0.2), 0 0 0 0 rgba(0,0,0,0.19);");
  str += F("}");
  
  str += F("</style>");
  str += F("</head>");
  str += F("<body>");
  
  str += F("<div class='align_center'><h2>ВЕНТИЛЯЦИЯ В ТЕПЛИЦЕ</h2></div><hr />");
  
  // *** Показания датчиков 
  str += F("<p><font class='sensors_txt'>Температура воздуха: </font>");
  str += F("<font class='sensors_val'>");
  str += String(temp);
  str += F(" град.С </font></p>");
  str += F("<p><font class='sensors_txt'>Влажность воздуха: </font>"); 
  str += F("<font class='sensors_val'>");
  str += String(hum);
  str += F(" % </font></p>"); 
  
  // *** Состояние вентиляции
  str += F("<p><font class='ctrl_txt'> Вентиляция: </font>");
  if(digitalRead(fan))  str += F("<font class='ctrl_on'> ВКЛЮЧЕНА");
  if(!digitalRead(fan)) str += F("<font class='ctrl_off'> ВЫКЛЮЧЕНА");
  str += F("</font></p><br/>");

  // *** Кнопки управления вентилятором ВРУЧНУЮ
  if (ctrl == 2) {
    str += F("<a href='/ventilation_ctrl?cmd=on'><button class='ctrl_on'> ВКЛЮЧИТЬ </button></a>&nbsp");
    str += F("<a href='/ventilation_ctrl?cmd=off'><button class='ctrl_off'> ВЫКЛЮЧИТЬ </button></a></p>");
  }
  str += F("<hr />");

  // *** Уставки для включения/выключения вентиляции
  str += F("<div class='align_center'>УСТАВКИ УПРАВЛЕНИЯ ВЕНТИЛЯЦИЕЙ В АВТОМАТИЧЕСКОМ РЕЖИМЕ</div><hr />");
  str += F("<form  method='POST' action='/ventilation_set'>");
  str += F("<p><font class='setting_txt'> температура =</font>");
  str += F("<input type='TEXT' class='setting_val'  name='set_temp' value=' "); 
  str += float_to_String(set_temp);
  str += F("\n'/> +/- 1.0 град.С </p>");
  str += F("<p><font class='setting_txt'> влажность &nbsp&nbsp < </font>");
  str += F("<input type='TEXT' class='setting_val' name='set_hum' value=' ");
  str += float_to_String(set_hum);
  str +=F("\n'/>  % </p>");
  str +=F("<div class='align_center'>");
  str += F("<p><input type='SUBMIT' class='set_button' value='ПРИМЕНИТЬ УСТАВКИ' /></p>");
  str += F("</form>");
  str += F("<p><a href='/' class='home_button'> НА ГЛАВНЫЙ ЭКРАН </a></p>");
  str += F("</div>");        
  str += F("</body>");
  str += F("</html>\n\r");
  server.send ( 200, "text/html", str );
  delay(1000); 
}

// *** Управление вентиляцией ВРУЧНУЮ 
void ventilation_ctrl() {
  if (server.arg(0)=="on") man_fan = true;  
  if (server.arg(0)=="off") man_fan = false; 
  server.send ( 200, "text/html", root_str );   
  delay(1000);
}

// *** Изменить уставки включения вентиляции 
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
  server.send ( 200, "text/html", ventilation_str ); 
  delay(1000);
}



//-----------------------------------------------------------------------------  
//             WEB-СЕРВЕР. НАСТРОЙКИ УПРАВЛЕНИЯ ПОЛИВОМ
//-----------------------------------------------------------------------------
void irrigation_adj(){
  String str = "";
  str += F("<!DOCTYPE HTML>");
  str += F("<html>");
  str += F("<head>");
  str += F("<meta charset='UTF-8' name='viewport' content='width=device-width, initial-scale=1'>");
  str += F("<title>НАСТРОЙКА УПРАВЛЕНИЯ ПОЛИВОМ</title>");
  str += F("<style type='text/css'>");
  
  str += F("body {"); 
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
  str += F(" width: 165px;");
  str += F("}");
    
  str += F(".ctrl {");
  str += F(" line-height: 36px;");
  str += F(" display: inline-block;");
  str += F(" width: 160px;");
  str += F(" position: relative;");
  str += F(" text-align: center;");
  str += F(" text-decoration: none;");
  str += F(" color: #FFF;");
  str += F(" background-color: #006778;");
  str += F(" background-image: -webkit-linear-gradient(top, #0087b0, #006778);");
  str += F(" background-image: linear-gradient(to bottom, #0087b0, #006778);");
  str += F(" border-bottom: solid 3px #004462;");
  str += F(" border-radius: 4px;");
  str += F(" box-shadow: inset 0 1px 0 rgba(255,255,255,0.2), 0 3px 2px rgba(0, 0, 0, 0.19);");
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

  str += F(".ctrl_auto{");
  str += F(" width: 160px;");
  str += F(" background-color: #006778;");
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

  str += F(".ctrl_auto:active {");
  str += F(" position:relative;");
  str += F(" top:2px;");
  str += F(" box-shadow: 0 0 0 0 rgba(0,0,0,0.2), 0 0 0 0 rgba(0,0,0,0.19);");
  str += F("}");

  str += F(".sensors_txt {");
  str += F(" display: inline-block;");
  str += F(" width: 200px;");
  str += F("}");
    
  str += F(".sensors_val {");
  str += F(" line-height: 36px;");
  str += F(" display: inline-block;");
  str += F(" width: 125px;");
  str += F(" position: relative;");
  str += F(" text-align: center;");
  str += F(" text-decoration: none;");
  str += F(" color: #FFF;");
  str += F(" background-color: #006778;");
  str += F(" background-image: -webkit-linear-gradient(top, #0087b0, #006778);");
  str += F(" background-image: linear-gradient(to bottom, #0087b0, #006778);");
  str += F(" border-bottom: solid 3px #004462;");
  str += F(" border-radius: 4px;");
  str += F(" box-shadow: inset 0 1px 0 rgba(255,255,255,0.2), 0 3px 2px rgba(0, 0, 0, 0.19);");
  str += F("}");

  str += F(".setting_txt {");
  str += F(" display: inline-block;");
  str += F(" width: 165px;");
  str += F("}");
 
  str += F(".setting_val_{");
  str += F(" line-height: 36px;");
  str += F(" background-color: #00cccc;");
  str += F(" display: inline-block;");
  str += F(" width: 80px;");
  str += F(" position: relative;");
  str += F(" text-align: center;");
  str += F(" color: #000000;");
  str += F(" border-radius: 4px;");
  str += F(" font-size: 12pt;"); 
  str += F("}");
  
  str += F(".setting_val {");
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

  str += F(".setting_val:focus {");
  str += F("  box-shadow: 0 0 5px #276873;");
  str += F("  padding: 3%;");
  str += F("  border: 2px solid #276873;");
  str += F("}");

  str += F(".set_button {");
  str += F(" background-color: #006778;");
  str += F(" border: none;");
  str += F(" box-shadow: 0 1px 8px 0 rgba(0,0,0,0.2), 0 6px 20px 0 rgba(0,0,0,0.19);");
  str += F(" -moz-border-radius:4px;");
  str += F(" -webkit-border-radius:4px;");
  str += F(" border-radius:4px;");
  str += F(" color: white;");
  str += F(" padding: 10px 30px;");
  str += F(" text-decoration: none;");
  str += F(" display: inline-block;");
  str += F(" font-size: 120%;");
  str += F(" margin-top:24px;");
  str += F("}");
  
  str += F(".set_button:active {");
  str += F(" position:relative;");
  str += F(" top:2px;");
  str += F(" box-shadow: 0 0 0 0 rgba(0,0,0,0.2), 0 0 0 0 rgba(0,0,0,0.19);");
  str += F("}");

  str += F(".home_button {");
  str += F(" background-color: #464e7a;");
  str += F(" border: none;");
  str += F(" box-shadow: 0 1px 8px 0 rgba(0,0,0,0.2), 0 6px 20px 0 rgba(0,0,0,0.19);");
  str += F(" -moz-border-radius:4px;");
  str += F(" -webkit-border-radius:4px;");
  str += F(" border-radius:4px;");
  str += F(" color: white;");
  str += F(" padding: 10px 30px;");
  str += F(" text-decoration: none;");
  str += F(" display: inline-block;");
  str += F(" font-size: 120%;");
  str += F(" margin-top:24px;");
  str += F("}");
  
  str += F(".home_button:active {");
  str += F(" position:relative;");
  str += F(" top:2px;");
  str += F(" box-shadow: 0 0 0 0 rgba(0,0,0,0.2), 0 0 0 0 rgba(0,0,0,0.19);");
  str += F("}");
  
  str += F("</style>");
  str += F("</head>");
  str += F("<body>");

  str += F("<div class='align_center'><h2>ПОЛИВ В ТЕПЛИЦЕ</h2></div><hr />");

  // *** Показания датчиков 
  /*
  str += F("<p><font class='sensors_txt'>Влажность почвы: </font>"); 
  str += F("<font class='sensors_val'>");
  str += String(moist);
  str += F(" % </font></p>");
  */

  after_irrig = now() - end_irrig; // прошло времени после окончания полива 
  str += F("<p>После окончания предыдущего полива </p>");
  str += F("<p><font class='ctrl_txt'> прошло: </font><font class='ctrl' >");
  str += String(day(after_irrig)-1);
  str += F(" дн.");
  str += String(hour(after_irrig));
  str += F("  час.");  
  str += String(minute(after_irrig));
  str += F("  мин.</font></p>");
 
  // *** клапан ОТКРЫТ
  if(valve_cmd()) {  
    irrig_cont = now() - start_irrig; // определение продолжительности  полива
    str += F("<p><font class='ctrl_txt'> От начала полива : </font><font class='ctrl'>");
    if (day(irrig_cont) > 1){
      str += String(day(irrig_cont)-1);
      str += F(" дн.");
    }
    str += String(hour(irrig_cont));
    str += F(" час.");
    str += digits_to_String(minute(irrig_cont));
    str += F(" мин.</font></p>");
    str += F("<p><font class='ctrl_txt'> Клапан полива: </font>");
    str += F("<font class='ctrl_on'> ОТКРЫТ");
  }

  // *** клапан ЗАКРЫТ
  if(!valve_cmd()) {  
    str += F("<p><font class='ctrl_txt'> Продолжительность : </font><font class='ctrl'>");
    if (day(irrig_cont) > 1){
      str += String(day(irrig_cont)-1);
      str += F(" дн.");
    }
    str += String(hour(irrig_cont));
    str += F(" час.");
    str += digits_to_String(minute(irrig_cont));
    str += F(" мин.</font></p>");
    str += F("<p><font class='ctrl_txt'> Клапан полива: </font>");
    str += F("<font class='ctrl_off'> ЗАКРЫТ");
  }  
  str += F("</font></p><br/>");

  // *** Кнопки управления клапаном полива ВРУЧНУЮ
  if (ctrl == 2) {
    str += F("<a href='/irrigation_ctrl?cmd=on'><button class='ctrl_on'>ОТКРЫТЬ</button></a>&nbsp");
    str += F("<a href='/irrigation_ctrl?cmd=off'><button class='ctrl_off'>ЗАКРЫТЬ</button></a></p>");
  }
  str += F("<hr />");

  // *** Настройки управления клапаном полива
  str += F("<div class='align_center'>УСТАВКИ УПРАВЛЕНИЯ ПОЛИВОМ В АВТОМАТИЧЕСКОМ РЕЖИМЕ</div><hr />");
  str += F("<form  method='POST' action='/irrigation_set'>");
  /*
  str += F("<p><font class='setting_txt'> влажность почвы = </font>");
  str += F("<input type='TEXT' class='setting_val'  name='set_moist' value=' ");
  str += float_to_String(set_moist);
  str += F("\n'/> +/- 5.0  % </p>");
  str += F("<p><div class='align_center'><u> КАЛИБРОВКА ДАТЧИКА ВЛАЖНОСТИ ПОЧВЫ </u></div></p>");
  str += F("<p><font class='setting_txt'> Выход датчика : </font>");
  str += F("<font class='sensors_val'>"); 
  str += analog_sensor;
  str += F(" ед. </font></p>");
  str += F("<p><font class='setting_txt'> значение для 0 % = </font>");         
  str += F("<input type='TEXT' class='setting_val' name='air_val' value=' ");
  str += air_val;
  str += F("\n'/> &nbsp ед. </p>");
  str += F("<p><font class='setting_txt'> значение для 100 % = </font>");        
  str += F("<input type='TEXT' class='setting_val' name='water_val' value=' ");
  str += water_val;
  str += F("\n'/> &nbsp ед. </p><br />");
  */



    
  str +=F("<div class='align_center'>");
  str += F("<p><input type='SUBMIT' class='set_button' value='ПРИМЕНИТЬ УСТАВКИ' />");
  str += F("</form>");
  str += F("<a href='/' class='home_button'> НА ГЛАВНЫЙ ЭКРАН </a>");
  str += F("</div>");        
  str += F("</body>");
  str += F("</html>\n\r");
  server.send ( 200, "text/html", str );
  delay(1000);
}

// *** Управление клапаном полива ВРУЧНУЮ 
void irrigation_ctrl() {
  if (server.arg(0)=="on") man_valve = true;  
  if (server.arg(0)=="off") man_valve = false;  
  //handleRoot();
  server.send ( 200, "text/html", root_str );
  delay(1000);
}

// *** Изменить уставки управления поливом
void irrigation_set(){
  set_moist = String_to_float(server.arg(0));
  if (set_moist < 10.0)  set_moist = 10.0;
  if (set_moist > 100.0)  set_moist = 100.0;
  air_val = server.arg(1).toInt();
  water_val = server.arg(2).toInt();
  Serial.print(F("set_moist = "));
  Serial.println(set_moist);
  Serial.print(F("air_val = "));
  Serial.println(air_val);
  Serial.print(F("water_val = "));
  Serial.println(water_val);
  //irrigation_adj();
  server.send ( 200, "text/html", irrigation_str ); 
  delay(1000);
}  

//------------------------------------------------------------------------------
//                 ИЗМЕНЕНИЕ НАСТРОЕК WiFi И MQTT БРОКЕРА 
//------------------------------------------------------------------------------
void WiFi_update() {
  
  // *** сканирование доступных WiFi сетей
  Serial.println("scan start");
  int n = WiFi.scanNetworks(false, true);
  ssid_set = "";
  wifi_scan = "";
  if (n == 0) {
    Serial.println("no networks found");
    wifi_scan += F("<p><font size='4'>");
    wifi_scan += F("нет доступных WiFi сетей");
    wifi_scan += F("</font></p>\n\r");
  }
  else {
    Serial.print(n);
    Serial.println(" networks found");
    wifi_scan += F("<p><font size='4'>");
    wifi_scan += F("найдено сетей WiFi ");
    wifi_scan += (n);
    wifi_scan += F("</font></p>\n\r");    
    for (int i = 0; i < n; ++i) {
      String ssid_scan = WiFi.SSID(i);
      if (ssid_scan.length() == 0)  ssid_scan = "< ??? >";
      Serial.print(i + 1);
      Serial.print(": ");
      Serial.print(ssid_scan);
      Serial.print(" (");
      Serial.print(WiFi.RSSI(i));
      Serial.print(")");
      Serial.println((WiFi.encryptionType(i) == ENC_TYPE_NONE) ? "   " : " *");
      ssid_set += F("<p><input type='radio' name='ssid' value='");
      ssid_set += String(WiFi.SSID(i));
      //ssid_set += F("\n");
      ssid_set += F("'"); 
      ssid_set += F("/> ");
      ssid_set += String(i + 1);
      ssid_set += F(": ");
      ssid_set += ssid_scan;
      ssid_set += F(" ( ");
      //ssid_set += String(WiFi.RSSI(i));
      ssid_set += String(toWiFiQuality(WiFi.RSSI(i)));
      ssid_set += F(" % )");
      ssid_set += ((WiFi.encryptionType(i) == ENC_TYPE_NONE)?" ":"*");
      ssid_set += F("</p>");
      delay(20);
    }
  }
  delay(100);  
  
  // *** страница WEB-сервера
  String str = "";
  str += "<!DOCTYPE HTML>\
          <html>\
          <head>\
          <meta charset='UTF-8' name='viewport' content='width=device-width, initial-scale=1'>\
          <title>WiFi BigGreenHouse</title>\
          <style>body { background-color: #FFFFFF; font-family: Arial, Helvetica, Sans-Serif; Color: #000000; font-size: 15pt}</style>\
          <style>.b1 {background: red; color: white; font-size: 15pt }</style>\
          </head>";
  str += F("<p><b>Доступные сети</b></p><body><form method='POST' action='ok'>");
  //str += F("<input type='text' name='ssid' /> WIFI SSID</br></br>");
  str += ssid_set;
  str += F("Текущий профиль WiFi: ");
  if(len_ssid < 32 && len_ssid != 0){
    str += ssid;
    str += F("</br></br>");
  }
  else    str += F("----------</br></br>");
  str += F("Пароль для выбранной сети : ");
  str += "</br></br>\
          <input type='text' name='pswd' /> WiFi PASSWORD\
          </br></br>\
          Настройки  MQTT брокера :\
          </br></br>\
          <input type='text' name='mqtt_server' />MQTT SERVER URL\
          </br></br>\
          <input type='text' name='mqtt_port' />MQTT PORT\
          </br></br>\
          <input type='text' name='mqtt_user' /> MQTT USER\
          </br></br>\
          <input type='text' name='mqtt_pass' /> MQTT PASSWORD\
          </br></br>\
          <input type=SUBMIT value='Cохранить настройки'>\
          </form>\
          <form method='GET' action='/upd'>\
          <input type='submit' value='Обновить ПО' /> Текущая версия ПО: ";
  str +=  String(FW_VERSION);
   
  str += "</form>\
          <form method='GET' action='/clear_wifi_setup'>\
          <input type='submit' class='b1' value='Очистить настройки !!!' />\
          </form>\
          <form method='GET' action='/'>\
          <input type='submit' value='Состояние теплицы' />\
          </form>\
          </body>\
          </html>";
  server.send ( 200, "text/html", str );
  delay(1000);
}

//------------------------------------------------------------------------------
//                СКАНИРОВАНИЕ WiFi И ВЫБОР СЕТИ ДЛЯ ПОДКЛЮЧЕНИЯ
//------------------------------------------------------------------------------
void WiFi_setup() {
  String str = "";
  str += F("<!DOCTYPE html>");
  str += F("<html>");
  str += F("<head>");
  str += F("<meta charset='UTF-8' name='viewport' content='width=device-width, initial-scale=1'>");
  str += F("<title>Доступные WiFi сети</title>");
  str += F("<style type='text/css'>");

  str += F("body {");
  str += F(" display: inline-block;");
  //str += F(" text-align: center;");
  str += F(" font-family:'Arial',sans-serif;");
  str += F(" margin-left: 20px;");
  str += F("}");

  str += F(".align_center {");
  str += F(" text-align: center;");
  str += F("}");
  
  str += F(".link_button {");
  //str += F(" background-color: #e4685d;");
  str += F(" background-color: #006778;");
  str += F(" border: none;");
  str += F(" box-shadow: 0 1px 8px 0 rgba(0,0,0,0.2), 0 6px 20px 0 rgba(0,0,0,0.19);");
  str += F(" -moz-border-radius:4px;");
  str += F(" -webkit-border-radius:4px;");
  str += F(" border-radius:4px;");
  str += F(" color: white;");
  str += F(" padding: 10px 30px;");
  str += F(" text-decoration: none;");
  str += F(" display: inline-block;");
  str += F(" font-size: 120%;");
  str += F(" margin-top:24px;");
  str += F("}");
  
  str += F(".link_button:active {");
  str += F(" position:relative;");
  str += F(" top:2px;");
  str += F(" box-shadow: 0 0 0 0 rgba(0,0,0,0.2), 0 0 0 0 rgba(0,0,0,0.19);");
  str += F("}");

  str += F(".home_button {");
  str += F(" background-color: #464e7a;");
  str += F(" border: none;");
  str += F(" box-shadow: 0 1px 8px 0 rgba(0,0,0,0.2), 0 6px 20px 0 rgba(0,0,0,0.19);");
  str += F(" -moz-border-radius:4px;");
  str += F(" -webkit-border-radius:4px;");
  str += F(" border-radius:4px;");
  str += F(" color: white;");
  str += F(" padding: 10px 30px;");
  str += F(" text-decoration: none;");
  str += F(" display: inline-block;");
  str += F(" font-size: 120%;");
  str += F(" margin-top:24px;");
  str += F("}");
  
  str += F(".home_button:active {");
  str += F(" position:relative;");
  str += F(" top:2px;");
  str += F(" box-shadow: 0 0 0 0 rgba(0,0,0,0.2), 0 0 0 0 rgba(0,0,0,0.19);");
  str += F("}");
  
  str += F(".ssid_list {");
  str += F(" font-weight:bold;");
  str += F(" line-height: 36px;");
  str += F(" margin-bottom: 12px;");
  str += F(" vertical-align: middle;");
  str += F("}");
  
  str += F(".ssid_list > a {");
  str += F(" width: 200px;");
  str += F(" display: inline-block;");
  str += F(" position: relative;");
  str += F(" padding: 0.25em 1.0em;");
  str += F(" margin-right: 1.0em;");
  str += F(" text-align: center;");
  str += F(" text-decoration: none;");
  str += F(" color: #FFF;");
  str += F(" background-color: #006778;");
  str += F(" background-image: -webkit-linear-gradient(top, #0087b0, #006778);");
  str += F(" background-image: linear-gradient(to bottom, #0087b0, #006778);");
  str += F(" border-bottom: solid 3px #004462;");
  str += F(" border-radius: 4px;");
  str += F(" box-shadow: inset 0 1px 0 rgba(255,255,255,0.2), 0 3px 2px rgba(0, 0, 0, 0.19);");
  str += F("}");
  
  str += F(".ssid_list > a:active {");
  str += F(" border-bottom: solid 3px #006778;");
  str += F(" box-shadow: 0 0 1px rgba(0, 0, 0, 0.2);");
  str += F("}");
  
  str += F(".img_lock {");
  str += F(" display: inline-block;");
  str += F(" width: 32px;");
  str += F(" height:32px;");
  str += F("  margin-left: 0.5em;");
  str += F("  vertical-align: middle;");
  str += F("  background: url(data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAACXBIWXMAAAsTAAALEwEAmpwYAAACZElEQVRYhe2VTUhUURTHf2emSaiNUi4KtKACTQgcJ8rJpC+CIMiF9uGmVViCU9kHRC3cRFCQqWiFEgR9QW2KVi1KKsescYRo1RdFSwuNCLNp3mkxd8DCeXNt5uGmPxzefeede87vnXvvezDLkplOqOl5WeQkEg0iBFW0BEBUPqkS9wUCt582rxrzBGBD+0jhT3/yLLAXYRSVh6AfTZoliG5CKQauzk36j/cfrhzPG0D4QrwWn3ML+KIqxwbHgg9oE+ePoDb1VRfFt4roOWABjm939FDwcc4A6zpiG1W4D1ycLODEcFMokX62vjNeDPAkEhxN+6ouxwIFk5wBDoiyfeBg6JFb/jnZABxkpQjt0ZaqUwBr20eW+vzJk0B9EqcQINwZGwfuOEn/6WdNlR+Ao+Gu4R8qWg64AsxoE1Z3vtgD0iswpMIlfDKSotRKUfYrrAHdNxhZfdM2pzVAdffwcknqK4TWaEuoZ7qYcFesGeU8UBGNhN7Z5rZWTUesdMptOdAA7AIq0s7a7ucleS/8l8qBIUCBr8C4GQ+ZZ56qHpgArptiYmyF8U0AO70qvhgYA1pdYo6YmEVeANwgdaTcNq4A/cC1fBcPAN+BRovYRuAbFt+YmaiU1EYrs4gtM7FWp8FnCTDPXD9bxKZj5lvmdlUfqbfJxfrcCmTrgP47u51sl2DWALJ14L0xzwAyKQHsAJYZqwN+eQGQqQNXgHtT7u8aX94BMum1pS9ngEwd2DKNb7MXAJm0jdS6p1VnfHkHcDsFCzOM8wrgubL9sdw60GssJ/3/FGdbgrfAQI413uQ431v9Bn8ZuSAzPe5xAAAAAElFTkSuQmCC) no-repeat;");
  str += F("}");

  str += F("</style>");
  str += F("</head>");
  str += F("<body>");
    
  // *** сканирование WiFi сетей
  Serial.println("scan start");
  int n = WiFi.scanNetworks(false, true);
  
  // *** список доступных WiFi сетей
  String ssid_list = "";
  if (n == 0) ssid_list += F("<div class='align_center'><h2>СЕТИ  WiFi НЕ НАЙДЕНЫ</h2></div><hr />");
  else {
    ssid_list += F("<div class='align_center'><h2>НАЙДЕНЫ СЕТИ  WiFi</h2></div><hr />");
    for (int i = 0; i < n; ++i) {
      String ssid_scan = WiFi.SSID(i);
      if (ssid_scan.length() == 0)  ssid_scan = "< ??? >";
      
      Serial.print(i + 1);
      Serial.print(": ");
      Serial.print(ssid_scan);
      Serial.print(" (");
      Serial.print(WiFi.RSSI(i));
      Serial.print(")");
      Serial.println((WiFi.encryptionType(i) == ENC_TYPE_NONE) ? "   " : " *");
      
      ssid_list += F("<div class='ssid_list'><a href='/wifi_entry?ssid=");
      ssid_list += ssid_scan == "< ??? >" ? "%3F" : ssid_scan;
      ssid_list += F("&psk_type="); 
      ssid_list += String(WiFi.encryptionType(i));
      ssid_list += F("'>");
      ssid_list += ssid_scan;
      ssid_list += F("</a>");
      ssid_list += String(toWiFiQuality(WiFi.RSSI(i)));
      ssid_list +=F("%");
      if (WiFi.encryptionType(i) != ENC_TYPE_NONE) ssid_list += F("<span class='img_lock' />");
      ssid_list += F("</div>");
    }
    ssid_list += F("<hr />");
    str += ssid_list; 
  }
  // *** кнопки в нижней части экрана
  str += F("<div class='align_center'>");
  str += F("<p><a href='/wifi_setup' class='link_button'>ПОВТОРИТЬ ПОИСК</a></p>");
  str += F("<p><a href='/' class='home_button'> НА ГЛАВНЫЙ ЭКРАН </a></p>");
  str += F("</div>");
  str += F("</body>");
  str += F("</html>");
  server.send ( 200, "text/html", str );
  delay(1000);
}


//------------------------------------------------------------------------------
//                       ПОДКЛЮЧЕНИЕ К ВЫБРАННОЙ СЕТИ WiFi  
//------------------------------------------------------------------------------
void WiFi_entry() {
  String str = "";
  str += F("<!DOCTYPE html>");
  str += F("<html>");
  str += F("<head>");
  str += F("<meta charset='UTF-8' name='viewport' content='width=device-width, initial-scale=1'>");
  str += F("<title>ПОДКЛЮЧЕНИЕ К СЕТИ WiFi</title>");
  str += F("<style type='text/css'>");
  
  str += F("body {");
  str += F("  display: inline-block;");
  str += F("  text-align: center;");
  str += F("  font-family:'Arial',sans-serif;");
  str += F("}");
  
  str += F(".form-style {");
  str += F(" display: inline-block;");
  str += F(" text-align: center;");
  str += F("  max-width: 400px;");
  str += F("  margin: 10px auto;");
  str += F("  padding: 16px;");
  str += F("}");
  
  str += F(".form-style h1{");
  str += F("  background: #006778;");
  str += F("  border-radius:4px;");
  str += F("  padding: 16px 0;");
  str += F("  font-size: 140%;");
  str += F("  font-weight: 300;");
  str += F("  text-align: center;");
  str += F("  text-shadow:0px 1px 0px #002778;");
  str += F("  color: #fff;");
  str += F("  margin: -16px -16px 16px -16px;");
  str += F("}");
  
  str += F(".form-style input[type='TEXT'],");
  str += F(".form-style textarea,");
  str += F(".form-style select ");
  str += F("{");  
  str += F("  text-align: center;");  // lm
  str += F("  -webkit-transition: all 0.30s ease-in-out;");
  str += F("  -moz-transition: all 0.30s ease-in-out;");
  str += F("  -ms-transition: all 0.30s ease-in-out;");
  str += F("  outline: none;");
  str += F("  box-sizing: border-box;");
  str += F("  -webkit-box-sizing: border-box;");
  str += F("  -moz-box-sizing: border-box;");
  str += F("  width: 100%;");
  str += F("  background: #fff;");
  str += F("  margin-bottom: 4%;");
  str += F("  border: 1px solid #ccc;");
  str += F("  padding: 3%;");
  str += F("  color: #555;");
  str += F("  font-size: 100%;");
  str += F("}");
  
  str += F(".form-style input[type='TEXT']:focus,");
  str += F(".form-style textarea:focus,");
  str += F(".form-style select:focus");
  str += F("{");
  str += F("  box-shadow: 0 0 5px #276873;");
  str += F("  padding: 3%;");
  str += F("  border: 2px solid #276873;");
  str += F("}");
  
  str += F(".form-style input[type='SUBMIT'] {"); 
  //str += F("  background-color: #e4685d;");
  str += F("  background-color: #006778;");
  str += F("  border: none;");
  str += F("  box-shadow: 0 1px 8px 0 rgba(0,0,0,0.2), 0 6px 20px 0 rgba(0,0,0,0.19);");
  str += F("  -moz-border-radius:4px;");
  str += F("  -webkit-border-radius:4px;");
  str += F("  border-radius:4px;");
  str += F("  color: white;");
  str += F("  padding: 10px 30px;");
  str += F("  text-decoration: none;");
  str += F("  display: inline-block;");
  str += F("  text-align: center;");
  str += F("  font-size: 120%;");
  str += F("  margin-top:24px;");
  str += F("}");
  
  str += F(".form-style input[type='SUBMIT']:active {");  
  str += F("  position:relative;");
  str += F("  top:2px;");
  str += F("  box-shadow: 0 0 0 0 rgba(0,0,0,0.2), 0 0 0 0 rgba(0,0,0,0.19);");
  str += F("}");
  
  str += F(".home_button {");
  str += F(" background-color: #464e7a;");
  str += F(" border: none;");
  str += F(" box-shadow: 0 1px 8px 0 rgba(0,0,0,0.2), 0 6px 20px 0 rgba(0,0,0,0.19);");
  str += F(" -moz-border-radius:4px;");
  str += F(" -webkit-border-radius:4px;");
  str += F(" border-radius:4px;");
  str += F(" color: white;");
  str += F(" padding: 10px 30px;");
  str += F(" text-decoration: none;");
  str += F(" display: inline-block;");
  str += F(" font-size: 120%;");
  str += F(" margin-top:24px;");
  str += F("}");
  
  str += F(".home_button:active {");
  str += F(" position:relative;");
  str += F(" top:2px;");
  str += F(" box-shadow: 0 0 0 0 rgba(0,0,0,0.2), 0 0 0 0 rgba(0,0,0,0.19);");
  str += F("}");
  
  str += F("</style>");
  str += F("</head>");
  str += F("<body>");

  String s_ssid = server.arg(0);
  String s_psk_type = server.arg(1);

  Serial.println("=================================");
  Serial.print("s_ssid = ");
  Serial.println(s_ssid);
  Serial.print("s_psk_type = ");
  Serial.println(s_psk_type);
  Serial.println("=================================");

  str += F("<div class='form-style'>");
  str += F("<h1>ПОДКЛЮЧЕНИЕ К СЕТИ WiFi</h1>");
  str += F("<form action='wifi_sta' method='POST'>");
  
  str += F("<input type='TEXT' name='s_ssid' ");
  if (s_ssid == "?") str += F("placeholder='SSID' />");
  else {
    str += F("value='");
    str += s_ssid;
    str += F("' readonly />");
  }
  
  //if (s_psk_type != 
  
  str += F("<input type='TEXT' name='s_psk' placeholder='Password' />");
  
  str += F("<input type='SUBMIT' value='ПОДКЛЮЧИТЬСЯ' />");
  str += F("</form>");
  str += F("<p><a href='/' class='home_button'> НА ГЛАВНЫЙ ЭКРАН </a></p>");
  str += F("</div>");
  str += F("</body>");
  str += F("</html>");  
  
  server.send ( 200, "text/html", str );
  delay(1000);
}

//==== ФУНКЦИЯ СОХРАНЕНИЯ НАСТРОЕК WiFi И MQTT ================================
void handleOk() {
  String mqtt_server_;
  String mqtt_user_;
  String mqtt_pass_;
  String mqtt_port_;
  String str = "";
  str += "<html>\
          <meta charset='UTF-8' name='viewport' content='width=device-width, initial-scale=1'>\
          <head>\
          <title>WiFi BigGreenHouse</title>\
          <style>body { background-color: #FFFFFF; font-family: Arial, Helvetica, Sans-Serif; Color: #000000; font-size: 15pt}</style>\
          <style>.b1 {background: red; color: white; font-size: 15pt }</style>\
          </head>\
          <body>";
  ssid = server.arg(0);
  pass = server.arg(1);
  
  mqtt_server_ = server.arg(2);
  mqtt_port_ = server.arg(3);
  mqtt_user_ = server.arg(4);
  mqtt_pass_ = server.arg(5);
  if (ssid != 0) {
    //EEPROM.put(96, ssid.length()); 
    //EEPROM.put(97, pass.length());
    Serial.print("EEPROM.96 <--");   
    Serial.println(ssid.length());
    Serial.print("EEPROM.97 <--");   
    Serial.println(pass.length());
    
    if(mqtt_server_ !=0)  {
      //EEPROM.put(400, mqtt_server_.length());
      Serial.print("EEPROM.400 <--");   
      Serial.println(mqtt_server_.length());
    }
    if(mqtt_port_ !=0)  {
      //EEPROM.put(401, mqtt_port_.length());
      Serial.print("EEPROM.401 <--");   
      Serial.println(mqtt_port_.length());      
    }
    if(mqtt_user_ !=0)  {
      //EEPROM.put(402, mqtt_user_.length());
      Serial.print("EEPROM.402 <--");   
      Serial.println(mqtt_user_.length()); 
    }
    if(mqtt_pass_ !=0)  {
      //EEPROM.put(403, mqtt_pass_.length());
      Serial.print("EEPROM.403 <--");   
      Serial.println(mqtt_pass_.length()); 
    }
       
    for (byte i = 0; i < ssid.length(); i++) {
      //EEPROM.put(i, ssid.charAt(i));
      Serial.print("EEPROM.");
      Serial.print(i);
      Serial.print(" <-- "); 
      Serial.println(ssid.charAt(i)); 
    }
    
    for (byte i = 0; i < pass.length(); i++)  {
      EEPROM.put(i + 32, pass.charAt(i));
      Serial.print("EEPROM."); 
      Serial.print(i+32);
      Serial.print(" <-- ");  
      Serial.println(pass.charAt(i)); 
    }
          
    if(mqtt_server_ !=0){   
      for (byte i = 0; i <= mqtt_server_.length(); i++){
        //EEPROM.put(404 + i, mqtt_server_.charAt(i));
        Serial.print("EEPROM.");   
        Serial.print(404+i);
        Serial.print(" <-- ");
        Serial.println(mqtt_server_.charAt(i)); 
      }
    }
    if(mqtt_port_ !=0){   
      for (byte i = 0; i < mqtt_port_.length(); i++){
        //EEPROM.write(436 + i, mqtt_port_.charAt(i));  //оставляем 32 символа для mqtt_server выше
        Serial.print("EEPROM.");  
        Serial.print(436+i);
        Serial.print(" <-- "); 
        Serial.println(mqtt_port_.charAt(i)); 
      }
    }
     
    if(mqtt_user_ !=0){    
      for (byte i = 0; i <= mqtt_user_.length(); i++){
        //EEPROM.write(441 + i, mqtt_user_.charAt(i));  //оставляем 32 символа для mqtt_server выше и 5 символов для mqtt_port
        Serial.print("EEPROM.441"); 
        Serial.print(441+i);
        Serial.print(" <-- ");  
        Serial.println(mqtt_user_.charAt(i)); 
      }
    }
    
    if(mqtt_pass_ !=0){    
      for (byte i = 0; i <= mqtt_pass_.length(); i++){
        //EEPROM.write(457 + i, mqtt_pass_.charAt(i));  //оставляем 32 символа для mqtt_server выше и 5 символов для mqtt_port, и еще 16 символов для mqtt_user 
        Serial.print("EEPROM.");
        Serial.print(457+i);
        Serial.print(" <-- ");   
        Serial.println(mqtt_pass_.charAt(i)); 
      }
    }      
    //EEPROM.commit();
    //EEPROM.end();
    Serial.println("Configuration saved in FLASH");
    str += "<p>Конфигурация сохранена в FLASH-памяти</p></br></br>\
            <a href='/reboot'>Перезагрузить</a></br>";
  } 
  else {
    str += "Cети WiFi не найдены !!!</br>\
          <a href='/wifi_setup'>Вернуться</a> на страницу настроек</br>";
  }
  str += "</body></html>";
  server.send ( 200, "text/html", str );
  //delay(1000);
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
    
    
    server.on("/ok", handleOk);
    
    server.on("/ventilation_adj", ventilation_adj);
    server.on("/ventilation_set", ventilation_set);
    server.on("/ventilation_ctrl", ventilation_ctrl);
    
    server.on("/irrigation_adj", irrigation_adj);
    server.on("/irrigation_set", irrigation_set);
    server.on("/irrigation_ctrl", irrigation_ctrl);
    
    server.on("/ctrl_mode", ctrl_mode);
    server.on("/ctrl_mode_set", ctrl_mode_set);
    
    server.on("/date_time", date_time);
    server.on("/date_time_set", date_time_set);
    
    //server.on("/comm_update", comm_update);
    
    server.on("/wifi_update", WiFi_update); // не используется ???
    server.on("/wifi_setup", WiFi_setup);
    server.on("/wifi_entry", WiFi_entry);
    
    //server.on("/comm_update_set", comm_update_set);
    //server.on("/clear_WiFi_setup", clear_wifi_setup);
    
    server.on("/reboot", reboot);
    
    server.on("/upd", HTTP_GET, []() {
      server.sendHeader("Connection", "close");
      server.send(200, "text/html", serverIndex);
    });
    server.on("/update", HTTP_POST, []() {
      server.sendHeader("Connection", "close");
      server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
      handleRoot();
      delay(100);
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
      } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
          Update.printError(Serial);
        }
      } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) { //true to set the size to the current progress
          Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
        } else {
          Update.printError(Serial);
        }
        Serial.setDebugOutput(false);
      }
      yield();
    });
    
    server.begin();
    delay(1000);
   
}  
//-----------------------------------------------------------------------------
//                           MQTT CALLBACK
//-----------------------------------------------------------------------------
void callback(char* topic, byte* payload, unsigned int length) {
  //Serial.print("Message arrived [");
  //Serial.print(topic);
  //Serial.print("] ");
  for (int i=0;i<length;i++) {
    //Serial.print((char)payload[i]);
  }
  //Serial.println();
} 

//-----------------------------------------------------------------------------
//                         NTP И MQTT RECONNECT
//----------------------------------------------------------------------------- 
void reconnect() {
  // *** выполнять, если после предыдущего запроса прошло более 1 минуты
  if(((millis() - event_reconnect) >= 60000) || first_scan){  
    event_reconnect = millis(); 
    if (WiFi.status() == WL_CONNECTED) {  // если есть подключение к роутеру 
      //Serial.println(WiFi.localIP().toString());
      if (WiFi.localIP() == ap_IP) WiFi.mode(WIFI_STA); 
      if (timeStatus() == timeNotSet) setSyncProvider(getNtpTime);  // если время не было установлено, запрос к NTP-серверу
      if (client.connect("arduinoClient")) {  // ********* уточнить и изменить
        Serial.println("MQTT connected OK");
        //client.publish("outTopic","hello world"); // публикация на брокере
        //client.subscribe("inTopic");  // подписка на топики
      } 
      else {
        Serial.print("MQTT connected failed, rc=");
        Serial.println(client.state());
      }
    }
    else {
      WiFi.mode(WIFI_AP_STA);
      Serial.println("---reconnect---"); 
    }
  }
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
 
//-----------------------------------------------------------------------------
//                          ЗАПРОС К NTP-СЕРВЕРУ
//                        (TimeNTP_ESP8266WiFi.ino)
//-----------------------------------------------------------------------------
void sendNTPpacket(IPAddress &address)
{
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
//  ОБРАБОТКА ПРЕРЫВАНИЯ ОТ ТАЙМЕРА ПОЛИВА - ЛОКАЛЬНАЯ КОМАНДА ОТКРЫТЬ КЛАПАН
//-----------------------------------------------------------------------------
void ICACHE_RAM_ATTR ISR_tmr_opn() {
  if (ctrl==0 && !digitalRead(tmr_opn) && digitalRead(tmr_cls))   auto_valve = true;
}
//-----------------------------------------------------------------------------
//  ОБРАБОТКА ПРЕРЫВАНИЯ ОТ ТАЙМЕРА ПОЛИВА - ЛОКАЛЬНАЯ КОМАНДА ЗАКРЫТЬ КЛАПАН
//-----------------------------------------------------------------------------
void ICACHE_RAM_ATTR ISR_tmr_cls() {
  if (ctrl==0 && !digitalRead(tmr_cls) && digitalRead(tmr_opn))   auto_valve = false;
}
//-----------------------------------------------------------------------------
//                          СТАРТОВАЯ ИНИЦИАЛИЗАЦИЯ 
//-----------------------------------------------------------------------------
void setup() {
  // Serial port
  Serial.begin(115200);
  
  // *** GPIO MODE
  pinMode(fan, OUTPUT);  
  pinMode(valve, OUTPUT);
  pinMode(ESP_BUILTIN_LED, OUTPUT);  
  
  //pinMode(tmr_rst, INPUT);  // вывод для рестарта таймера
  pinMode(tmr_opn, INPUT_PULLUP); // ожидаение импульса низкого уровеня
  pinMode(tmr_cls, INPUT_PULLUP); // ожидаение импульса низкого уровеня
 
  // *** GPIO STATE
  //digitalWrite(tmr_rst, HIGH);
  digitalWrite(fan, LOW);
  digitalWrite(valve, LOW); // ... или восстановить состояние из EEPROM ?
  digitalWrite(ESP_BUILTIN_LED, LOW);
  attachInterrupt(digitalPinToInterrupt(tmr_opn), ISR_tmr_opn, CHANGE);  // активировать прерывание от таймера полива 
  attachInterrupt(digitalPinToInterrupt(tmr_cls), ISR_tmr_cls, CHANGE);  // активировать прерывание от таймера полива    

  // *** VARIABLE STATE
  ctrl = 0; // режим: auto
  rem_valve = false;  // дистанционная команда для реле клапана полива = выключить
  rem_fan = false;  // дистанционная команда для реле вентилятора = выключить
  auto_fan = false;  // локальная команда для реле вентилятора = выключить
  auto_valve = false;  // локальная команда для реле клапана полива = выключить  
  irrig_state = false;
  man_ctrl = false;
  man_fan = false;
  man_valve = false;
  
  
  // *** WiFi
  WiFi.softAPConfig (ap_IP, ap_gateway, ap_subnet);  // конфигурирование точки доступа
  WiFi.softAP(ap_ssid, ap_pass);
  
  WiFi.begin(ssid.c_str(), pass.c_str()); // инициализация подключения к роутеру
  byte tries = 5;
  while(--tries &&  WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(1000);
  }
  Serial.println("");

  // *** NTP - конфигурирование синхронизации времени 
  Udp.begin(localPort);
  setSyncProvider(getNtpTime);
  setSyncInterval(86400); // повторная синхронизация с NTP сервером каждые 86400 сек. = 24 час.
  
  // *** MQTT broker
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
  delay(1000);
  
  // *** WEB-сервер
  http_server();  
  
  // *** инициализация опроса BME280 через I2C
  Wire.begin();
  event_bme_begin = millis();
  while(!bme.begin() && millis() - event_bme_begin < 5000);
  if (!bme.begin()) Serial.println("BME280 не найден !!!");  
  ct_rd = 0;
    
  // *** EEPROM
  EEPROM.begin(512); // размер eeprom памяти 512 байт
  int eeprom = EEPROM.read(115);
  //Serial.print("EEPROM.115 = ");
  //Serial.println(eeprom);
  
  // проверка состояния данных в EEPROM
  // если в eeprom были ранее записаны данные, восстанавливаем значения из EEPROM
  // иначе, все значения остаются по умолчанию
  
  if(eeprom == 170)  {  // = B#10101010
    len_ssid = EEPROM.read(96);  
    len_pass = EEPROM.read(97);
    if (len_pass > 64) len_pass = 0; // ограничение длины пароля WiFi
    
    if ((len_ssid < 33) && (len_ssid != 0)) {   
      ssid = "";
      for (byte i = 0; i < len_ssid; i++) ssid += char(EEPROM.read(i));
      delay(1);
      pass = "";
      for (byte i = 0; i < len_pass; i++) pass += char(EEPROM.read(32 + i));
      delay(1);
    }
    set_temp = EEPROM_float_read(120); 
    set_hum = EEPROM_float_read(124);
    set_moist = EEPROM_float_read(128);   
    len_mqtt_server = EEPROM.read(400);
    len_mqtt_port = EEPROM.read(401);
    len_mqtt_user = EEPROM.read(402);
    len_mqtt_pass = EEPROM.read(403);
      
    mqtt_server = ""; // url mqtt сервера, но только если длинна len_mqtt_server менее 32 символа
    if(len_mqtt_server <= 32){  
      for(byte i = 0; i <= len_mqtt_server; i++)mqtt_server += char(EEPROM.read(404 + i));
    }
  
    String mqtt_port_ = ""; // mqqt порт, но только если длинна менее 6 символов  
    if(len_mqtt_port < 6){  
      for(byte i = 0; i < len_mqtt_port; i++)mqtt_port_ += char(EEPROM.read(436 + i));    
    }
    mqtt_port = mqtt_port_.toInt();
         
    if(len_mqtt_user <= 16){  // mqqt user 
    for(byte i = 0; i <= len_mqtt_user; i++)mqtt_user += char(EEPROM.read(441 + i));
    }
         
    if(len_mqtt_pass <= 32){  // mqqt pass
    for(byte i = 0; i <= len_mqtt_pass; i++)mqtt_pass += char(EEPROM.read(457 + i));
    }  
  }
  
  
    
  
/*    
    eeprom = 170;
    //EEPROM.write(115, eeprom);
      
    //EEPROM_float_write(120, set_temp);
    //EEPROM_float_write(124, set_hum);
    //EEPROM_float_write(128, set_moist);
    //EEPROM.commit();
    
    WiFi_Scan(); // сканирование сети
    delay(1000);
    WiFi_AP();  // запуск точки доступа 
    http_server();
  
  
  Serial.println();
  Serial.print("len_ssid = ");
  Serial.println(len_ssid);
    
  Serial.println();
  Serial.print("SSID: ");
  Serial.println(ssid);
  Serial.print("Password: ");
  Serial.println(pass);
  Serial.print(F("MQTT SERVER : "));
  Serial.println((String)mqtt_server);
  
  Serial.print(F("MQTT port : "));
  Serial.println(mqtt_port);
    
  Serial.print(F("MQTT user : "));
  Serial.println((String)mqtt_user);
  Serial.print(F("MQTT pass : "));
  Serial.println((String)mqtt_pass);
   
*/
  // *** подключение к NTP и MQTT
  if (WiFi.status() == WL_CONNECTED) reconnect();
   
  
  // *** инициализация таймеров событий
  mem = now();
  event_sensors_read = millis();
  event_serial_output = millis();
  event_reconnect = millis();  
  first_scan = true;
  irrig_sta = false;
  
  Serial.println("Ready");  //  "Готово" 
  digitalWrite(ESP_BUILTIN_LED, HIGH);
}

//-----------------------------------------------------------------------------
//                                ОСНОВНОЙ ЦИКЛ
//-----------------------------------------------------------------------------
void loop() {
  
  // *** режимы управления вентиляцией и поливом
  if (ctrl > 2) ctrl = 2;
  if (ctrl < 0) ctrl = 0;

  if (ctrl == 0) {
    man_fan = auto_fan;
    man_valve = auto_valve;
  }
  if (ctrl == 1) {
    man_fan = rem_fan;
    man_valve = rem_valve;
  }
  
  // *** проверка подключения к WiFi и МQTT брокеру и, при необходимости, переподключение
  reconnect();  // запрос к NTP серверу и подключение к MQTT-брокеру
   
  // *** опрос датчиков и формирование локальных команд
  sensors_read();

  // *** включение реле вентилятора 
  digitalWrite(fan, fan_cmd());

  // *** включение реле клапана полива 
  digitalWrite(valve, valve_cmd());
    
  // *** учет времени полива
  if (valve_cmd && !irrig_state) {  // старт полива
    start_irrig = now();
    irrig_state = true;
  }
 
  if (!valve_cmd && irrig_state) {  // окончание полива
    end_irrig = now();
    irrig_state = false;
  }

  // *** обработка запросов к WEB-серверу
  server.handleClient();
      
  // *** вывод информации в порт    
  if (millis() - event_serial_output > 20000) { // повторять через 20 секунд
    event_serial_output = millis();
    Serial.print("ctrl = ");  
    Serial.println(ctrl);
    if (!digitalRead(fan)) Serial.println("Реле вентилятора = ВЫКЛ.");  
    if (digitalRead(fan)) Serial.println("Реле вентилятора = ВКЛ."); 
    if (!digitalRead(valve)) Serial.println("Реле клапана воды = ВЫКЛ.");  
    if (digitalRead(valve)) Serial.println("Реле клапана воды = ВКЛ."); 
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
    Serial.println(((WiFi.status() == WL_CONNECTED))? state : "Не подключен к роутеру"); 
    Serial.print("MAC: ");
    Serial.println(WiFi.macAddress());
  }
  client.loop();  
  delay(1000);
  first_scan = false;
}
