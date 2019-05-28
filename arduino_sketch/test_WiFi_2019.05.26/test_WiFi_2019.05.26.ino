#define FW_VERSION "FW v.1.0. debag"

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

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
//#include <ESP8266HTTPUpdateServer.h>
#include <WiFiUdp.h> 
#include <PubSubClient.h>
#include <EEPROM.h>
#include <Wire.h>
#include <BME280I2C.h>
#include <ArduinoOTA.h>

// === WiFi AccessPoint === 
IPAddress local_IP(192,168,123,22);
IPAddress gateway(192,168,123,1);
IPAddress subnet(255,255,255,0);
const char *ap_ssid = "BigGreenHouse";
const char *ap_pass = "12345678";

// === WiFi Scan ===
String wifi_scan = "";
String ssid_set = "";

// === WiFi Client === 
String ssid = "Keenetic-2927";
String pass = "dUfWKMTh";

// === MQTT ===
//IPAddress mqtt_server(192, 168, 123, 222);
char* mqtt_server = "192.168.123.222";
int mqtt_port = 1883;
String mqtt_user = "";
String mqtt_pass = "";


byte step_mqtt_send = 0; // переменная для поочередной (по однаму сообщению за цикл loop) публикации сообщений на MQTT брокере
String id_messages_ = "";
boolean send_ = 0;

WiFiClient espClient;
PubSubClient client(espClient); 


// === WEB SERVER ===
ESP8266WebServer server(80);
const char* serverIndex = "<form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='update'><input type='submit' value='Update'></form>";
byte len_ssid, len_pass, len_mqtt_server, len_mqtt_port, len_mqtt_user, len_mqtt_pass;
const char* host = "OTA";

// === BME280 ===
BME280I2C bme;
float temp, pres, hum, moist;
float set_temp = 22.5;  // не ниже 15*С, оптимально 20...25*С
float set_hum = 50.0; // оптимальная влажность воздуха 45...60% 
float set_moist = 65.0; // оптимальная влажность почвы 60...70% или более

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

const int ESP_BUILTIN_LED = 2;

// === CTRL ===
byte last_irrig_day, last_irrig_hour, last_irrig_minute;
boolean ctrl = false; // режим: false = local, true = remote
boolean rem_valve = false;  // дистанционная команда для реле клапана полива: false = выключить, true = включить
boolean rem_fan = false;  // дистанционная команда для реле вентилятора: false = выключить, true = включить
boolean local_fan = false;  // локальная команда для реле вентилятора: false = выключить, true = включить
volatile boolean local_valve = false;  // локальная команда для реле клапана полива: false = выключить, true = включить




//----- ДЛЯ СИНХРОНИЗАЦИИ С NTP СЕРВЕРОМ -----
#define GMT 5 //часовой пояс
// IPAddress timeServer(129, 6, 15, 28);  // time.nist.gov NTP server
IPAddress timeServerIP;                   // time.nist.gov NTP server address
WiFiUDP udp;  // A UDP instance to let us send and receive packets over UDP
unsigned int localPort = 2390;      // local port to listen for UDP packets
/* Don't hardwire the IP address or we won't get the benefits of the pool.
    Lookup the IP address for the host name instead */
const char* ntpServerName = "time.nist.gov";
const int NTP_PACKET_SIZE = 48; // NTP time stamp is in the first 48 bytes of the message
byte packetBuffer[ NTP_PACKET_SIZE]; //buffer to hold incoming and outgoing packets
long eventClockNew;
long eventStart; //старт синхронизации часов
long highWord;
long lowWord;
long epoch;
long secsSince1900 = 0;
long secsSince1900_2 = 0; //unix time с 1900 года 1 яанваря (время последнего удачного онлайн обнавления).
byte state_clock = 1;
byte weekday = 4;// день недели
byte day = 1;// число месяца
int year = 1970;// год
byte month = 1;// месяц
String timeClock = ""; //часы и минуты
int month_days; //дней в месяце
long count_day; //количество дней с 1970 года 1 января по текущее время
long v_year; //кличество дней в году
long next_year; // дней с 1970 года 1 января, до наступления следующего года 
int next_month; // дней от начала текущего года до наступления следующего месяца
int count_day_curent_year; // счетчик дней текущего года
//int accuracy = 0; // буфер погрешности времени при подсчете оффлайн (в милисекундах)
String curent_time = ""; // вывод текущего времени и даты

// === ТАЙМЕРЫ СОБЫТИЙ ===
long eventTime; // для снятия текущего времени с начала старта
long event_mqtt_connect;  // для подключения к MQTT брокеру
long event_sensors_read; // для опроса датчиков
long event_bme_begin; // для подключения  датчика BME280
long event_serial_output;


//-----------------------------------------------------------------------------
//     ОПРОС ДАТЧИКОВ ТЕМПЕРАТРЫ И ВЛАЖНОСТИ. ЛОКАЛЬНЫЕ КОМАНДЫ ВЕНТИЛЯЦИИ
//-----------------------------------------------------------------------------
void sensors_read(){
  if((millis() - event_sensors_read) > 10000) {
    event_sensors_read = millis();

    if (!bme.begin()) Serial.println("BME280 не найден !!!");
    else Serial.println("BME280 подключен");
    
    // **** ОПРОС ДАТЧИКА BME280
    BME280::TempUnit tempUnit(BME280::TempUnit_Celsius);
    BME280::PresUnit presUnit(BME280::PresUnit_Pa);
    bme.read(pres, temp, hum, tempUnit, presUnit);  // считать значения давления, температуры, влажности, в метрической системе, давление в Ра 
    //  temp -= 0.3;                            // correct <temp>
    pres /= 133.3;                            // convert <pres> to mmHg

    // *** ЛОКАЛЬНАЯ КОМАНДА УПРАВЛЕНИЯ РЕЛЕ ВЕНТИЛЯТОРА
    if (temp >= set_temp + 2.5 || hum >= set_hum + 5.0) local_fan = true; // включить вентилятор, если жарко или влажно
    if (temp <= set_temp - 2.5 && hum <= set_hum - 5.0) local_fan = false;  // выключить вентилятор, если не жарко и влажность в норме 
    if (temp <= 16.0) local_fan = false;  // выключить вентилятор всегда, когда температура ниже 16 град.С
      
    if (!local_fan) Serial.println("Локальная команда реле вентилятора = ВЫКЛ.");  
    if (local_fan) Serial.println("Локальная команда реле вентилятора = ВКЛ."); 
    
  
    // **** ОПРОС ДАТЧИКА ВЛАЖНОСТИ ПОЧВЫ  
    analog_sensor = analogRead(0);
    int sensor_moist = analog_sensor;

    if (sensor_moist >= air_val) sensor_moist = air_val;
    if (sensor_moist <= water_val) sensor_moist = water_val;

    //moist = float(map(sensor_moist, air_val, water_val, 0, 100));
    moist = 100.0 * float(air_val-sensor_moist) / float(air_val-water_val);

    // *** ЛОКАЛЬНАЯ КОМАНДА ВЫКЛЮЧЕНИЯ РЕЛЕ КЛАПАНА ПОЛИВА
    if (moist >= set_moist + 5.0 &&  local_valve) local_valve = false; // выключить реле клапана полива, если почва достаточно влажная
   
  
    // *** ВЫВОД ИНФОРМАЦИИ С ДАТЧИКОВ В ПОРТ
    Serial.print ("Температура воздуха   ");
    Serial.print (temp);
    Serial.println ("   *C");
  
    Serial.print ("Влажность воздуха   ");
    Serial.print (hum);
    Serial.println ("   %");
  
    Serial.print ("Давление   ");
    Serial.print (pres);
    Serial.println ("   mmHg");

    Serial.print ("Влажность почвы   ");
    Serial.print (moist);
    Serial.print ("   %  ["); 
    Serial.print (analog_sensor);  
    Serial.println ("]"); 

  }
}



//-----------------------------------------------------------------------------
//                      ПРЕОБРАЗОВАНИE IPaddres В STRING 
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
//                          РЕЖИМ ТОЧКИ ДОСТУПА WiFi
//-----------------------------------------------------------------------------
void WiFi_AP() {
  //WiFi.softAPConfig (local_IP, gateway, subnet);
  //WiFi.softAP(ap_ssid, ap_pass);
  Serial.print("Setting soft-AP configuration ... ");
  Serial.println(WiFi.softAPConfig (local_IP, gateway, subnet) ? "Ready" : "Failed!");
  Serial.print("Setting soft-AP ... ");
  Serial.println(WiFi.softAP(ap_ssid, ap_pass) ? "Ready" : "Failed!");
  Serial.print("Soft-AP IP address = ");
  Serial.println(WiFi.softAPIP());
  WiFi.printDiag(Serial);
}


//-----------------------------------------------------------------------------
//                        РЕЖИМ СТАНЦИИ В СЕТИ WiFi 
//-----------------------------------------------------------------------------
void WiFi_STA() {
  WiFi.begin(ssid, pass);
  WiFi.config (local_IP, gateway, subnet);
  Serial.println("WiFi_STA_OK");
}



//-----------------------------------------------------------------------------  
//                СТРАНИЦА WEB-СЕРВЕРА СОСТОЯНИЯ ТЕПЛИЦЫ
//-----------------------------------------------------------------------------
void handleRoot(){
  String str = "";
  str += F("<html>\
            <head><meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">\
            <title>КЛИМАТ В ТЕПЛИЦЕ</title>\
            <style>\
              body {background-color: #FFFFFF; font-family: Arial, Helvetica, Sans-Serif; Color: #000000; font-size: 15pt;}\
              .btn {font-family: Arial, Helvetica, Sans-Serif; Color: #000000; font-size: 15pt;}\
            </style>\
            </head>\
            <body>\
            <hr />\
            <p><font size=\"2\"><b><u>СОСТОЯНИЕ НА </u>");
  str += String(curent_time);
  str += F("</b></font></p><hr />\
            <p>Режим управления : ");
  if (ctrl) str += F("<font color = \"#00a000\"> дистанционный</font></p>");
  else str += F("<font color = \"#0000FF\"> локальный</font></p><hr />");
  str += F("<p>Атмосферное давление .. ");
  str += String(pres);
  str += F(" mm.Hg </p>\
            <p>Температура воздуха ...... ");
  str += String(temp);
  str += F(" град.С </p>\
            <p>Влажность воздуха ......... ");
  str += String(hum);
  str += F(" % </p>");
  if(digitalRead(fan) == HIGH) {
    str += F("<p><font color = \"#00a000\"> >>> <u>ВЕНТИЛЯЦИЯ ВКЛЮЧЕНА!</u></font></p>");
  }
  if(digitalRead(fan) == LOW) {
    str += F("<p><font color = \"#0000FF\"> >>>> <u>ВЕНТИЛЯЦИЯ ВЫКЛЮЧЕНА!</u></font></p>");
  }
  str += F("<p>Влажность почвы ............ ");
  str += String(moist);
  str += F(" % </p>");
  if(digitalRead(valve)== HIGH) {
    str += F("<p><font color = \"#00a000\"> >>> <u>ИДЕТ ПОЛИВ!</u></font> </p>");
  }
  if(digitalRead(valve) == LOW) {
    str += F("<p><font color = \"#0000FF\"> >>>> <u>ПОЛИВ ВЫКЛЮЧЕН!</u></font> </p>");
  }
  str += F("<p>После окончания предыдущего полива прошло:</p><p>");
  str += String(last_irrig_day);
  str += F(" дн.");
  str += String(last_irrig_hour);
  str += F("  час.");  
  str += String(last_irrig_minute);
  str += F("  мин.</p><hr />\ 
            <form method=\"GET\" action=\"/setting\">\
            <input type=\"submit\" class=\"btn\" value=\"Изменить настройки управления вентиляцией и поливом\">\
            </form>\
            <br /><hr />\
            <form action=\"/WiFi_setup\" method=\"GET\">");
            if(WiFi.status() == WL_CONNECTED) {
              str += F("<p><font color = \"#00a000\"> >>>> <u>WiFi CONNECTED !</u></font></p>\
                        <p>SSID:<font color = \"#00a000\">");
              str += String(ssid);
              str += F("</p><p></font>IP addres: <font color = \"#00a000\">"); 
              str += WiFi.localIP().toString();
              str += F("</font></p><p>MQTT broker : ");
              str += String(mqtt_server);
              str += F("  port : ");
              str += String(mqtt_port);
              str += F("</p>");
              if(client.connect("BigGreenHouse")) {
                str += F("<p><font color = \"#00a000\"> >>>> <u>MQTT CONNECTED !</u></font></p>");
              }
              else {
                str += F("<p><font color = \"#FF0000\"> >>>> <u>MQTT NOT CONNECTED !!!</u></font></p>");
              }
            }
            else {
              str += F("<p><font color = \"#FF0000\"> >>>> <u>WiFi NOT CONNECTED !!!</u></font></p>");
            }     
            str += F("<hr /><input type=\"submit\" class=\"btn\" value=\"Изменить настройки WiFi и MQTT\" />\
            </form><br /><br />\
            </body>\
            </html>\n\r");
  server.send ( 200, "text/html", str );   
}

//-----------------------------------------------------------------------------  
//             СТРАНИЦА WEB-СЕРВЕРА НАСТРОЕК УПРАВЛЕНИЯ ТЕПЛИЦЕЙ 
//-----------------------------------------------------------------------------
void handleSet(){
  String str = "";
  str += F("<html>\
            <head><meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">\
            <title>НАСТРОЙКИ УПРАВЛЕНИЯ </title>\
            <style>\
              body {background-color: #FFFFFF; font-family: Arial, Helvetica, Sans-Serif; Color: #000000; font-size: 15pt;}\
              .btn {font-family: Arial, Helvetica, Sans-Serif; Color: #000000; font-size: 15pt;}\
            </style>\
            </head>\
            <body>\
            <hr />\
            <p>");
  str += F("<p>Режим управления : </p>\
            <form action=\"/ctrl_mode\" method=\"POST\" >\
            <input type=\"radio\" style=\"width: 50px; \" name=\"ctrl\" value=\"false\" "); 
  if(!ctrl)str += F("checked/> <font color = \"#0000FF\"><u>Локальный</u></font>");
  else str += F("/> Локальный");
  str += F("<input type=\"radio\" style=\"width: 50px; \" name=\"ctrl\" value=\"true\" ");
  if(ctrl)str += F("checked/> <font color = \"#00a000\"><u>Дистанционный</u></font>");
  else str += F("/> Дистанционный");
  str += F("<br /><br /><input type=\"submit\" class=\"btn\" value=\"Установить режим\" />\
            </form></p><hr />\
            <p><u> УСТАВКИ ТЕМПЕРАТУРЫ И ВЛАЖНОСТИ </u></p>\
            <form action=\"/ajustment\" method=\"POST\">\
            - температура воздуха ...\
            <input type=\"text\" style=\"width: 75px; font-size: 15pt;\" name=\"set_temp\" value=\" "); 
  str += float_to_String(set_temp);
  str += F("\n\"/> +/- 2.5 град.С <br /><br />\
            - влажность воздуха .......\
            <input type=\"text\" style=\"width: 75px; font-size: 15pt;\" name=\"set_hum\" value=\" ");
  str += float_to_String(set_hum);
  str +=F("\n\"/> +/- 10 %<br /><br />\
            - влажность почвы ..........\
            <input type=\"text\" style=\"width: 75px; font-size: 15pt;\" name=\"set_moist\" value=\" ");
  str += float_to_String(set_moist);
  str += F("\n\"/> +/- 5  %<br /><br />\
            калибровка датчика влажности почвы <br />\
            выход датчика = ");
  str += analog_sensor;
  str += F("ед. <br />\
            - значение для 0% ...\         
            <input type=\"text\" style=\"width: 75px; font-size: 15pt;\" name=\"air_val\" value=\" ");
  str += air_val;
  str += F("\n\"/> ед. \
            - значение для 100% ...\         
            <input type=\"text\" style=\"width: 75px; font-size: 15pt;\" name=\"water_val\" value=\" ");
  str += water_val;
  str += F("\n\"/> ед. <br /><br />\  
            <input type=\"submit\" class=\"btn\" value=\"Сохранить уставки\" />\
            </form>\
            <br /><hr />\
            <form action=\"/WiFi_setup\" method=\"GET\">");
            if(WiFi.status() == WL_CONNECTED) {
              str += F("<p><font color = \"#00a000\"> >>>> <u>WiFi CONNECTED !</u></font></p>\
                        <p>SSID:<font color = \"#00a000\">");
              str += String(ssid);
              str += F("</p><p></font>IP addres: <font color = \"#00a000\">"); 
              str += WiFi.localIP().toString();
              str += F("</font></p><p>MQTT broker : ");
              str += String(mqtt_server);
              str += F("  port : ");
              str += String(mqtt_port);
              str += F("</p>");
              if(client.connect("BigGreenHouse")) {
                str += F("<p><font color = \"#00a000\"> >>>> <u>MQTT CONNECTED !</u></font></p>");
              }
              else {
                str += F("<p><font color = \"#FF0000\"> >>>> <u>MQTT NOT CONNECTED !!!</u></font></p>");
              }
            }
            else {
              str += F("<p><font color = \"#FF0000\"> >>>> <u>WiFi NOT CONNECTED !!!</u></font></p>");
            }     
            str += F("<hr /><input type=\"submit\" class=\"btn\" value=\"Изменить настройки WiFi и MQTT\" />\
            </form><br /><br />\
            <form method=\"GET\" action=\"/\">\
            <input type=\"submit\" class=\"btn\" value=\"Вернуться на Главный экран\">\
            </form>\            
            </body>\
            </html>\n\r");
  server.send ( 200, "text/html", str );   
}


//------------------------------------------------------------------------------
//                  ИЗМЕНЕНИЕ РЕЖИМА УПРАВЛЕНИЯ ТЕПЛИЦЕЙ
//------------------------------------------------------------------------------
void ctrl_mode() {
  Serial.print("ctrl_mode... = ");
  Serial.println(server.arg(0));
  if (server.arg(0)=="true") ctrl = true;
  if (server.arg(0)=="false") ctrl = false;
  handleSet();
}


//------------------------------------------------------------------------------
//              ИЗМЕНЕНИЕ ЗНАЧЕНИЙ УСТАВОК УПРАВЛЕНИЯ ТЕПЛИЦЕЙ
//------------------------------------------------------------------------------

void ajustment(){
  Serial.println("ajustment...");
  set_temp = String_to_float(server.arg(0)); 
  set_hum = String_to_float(server.arg(1)); 
  set_moist = String_to_float(server.arg(2));
  air_val = server.arg(3).toInt();
  water_val = server.arg(4).toInt();
  
  Serial.print(F("set_temp = "));
  Serial.println(set_temp);


  Serial.print(F("set_hum = "));
  Serial.println(set_hum);

  Serial.print(F("set_moist = "));
  Serial.println(set_moist);

  Serial.print(F("air_val = "));
  Serial.println(air_val);

  Serial.print(F("water_val = "));
  Serial.println(water_val);

  
  handleSet();
}







//------------------------------------------------------------------------------
//                 ИЗМЕНЕНИЕ НАСТРОЕК WiFi И MQTT БРОКЕРА 
//------------------------------------------------------------------------------
void handleComm() {
  
  // *** сканирование доступных WiFi сетей
  Serial.println("scan start");
  int n = WiFi.scanNetworks();
  ssid_set = "";
  wifi_scan = "";
  if (n == 0) {
    Serial.println("no networks found");
    wifi_scan += F("<p><font size=\"4\">");
    wifi_scan += F("нет доступных WiFi сетей");
    wifi_scan += F("</font></p>\n\r");
  }
  else {
    Serial.print(n);
    Serial.println(" networks found");
    wifi_scan += F("<p><font size=\"4\">");
    wifi_scan += F("найдено сетей WiFi ");
    wifi_scan += (n);
    wifi_scan += F("</font></p>\n\r");    
    for (int i = 0; i < n; ++i) {
      Serial.print(i + 1);
      Serial.print(": ");
      Serial.print(WiFi.SSID(i));
      Serial.print(" (");
      Serial.print(WiFi.RSSI(i));
      Serial.print(")");
      Serial.println((WiFi.encryptionType(i) == ENC_TYPE_NONE) ? " " : "*");
      ssid_set += F("<input type=\"radio\" name=\"ssid\" value=\""); 
      ssid_set += String(WiFi.SSID(i));
      //ssid_set += F("\n");
      ssid_set += F("\""); 
      ssid_set += F("/> ");
      ssid_set += String(i + 1);
      ssid_set += F(": ");
      ssid_set += String(WiFi.SSID(i)); 
      ssid_set += F(" (");
      ssid_set += String(WiFi.RSSI(i));
      ssid_set += F(")");
      ssid_set += ((WiFi.encryptionType(i) == ENC_TYPE_NONE)?" ":"*");
      ssid_set += F("</br></br>");
      delay(10);
    }
  }
  delay(100);  
  
  // *** страница WEB-сервера
  String str = "";
  str += "<html>\
          <meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">\
          <head>\
          <title>WiFi BigGreenHouse</title>\
          <style>body { background-color: #FFFFFF; font-family: Arial, Helvetica, Sans-Serif; Color: #000000; font-size: 15pt}</style>\
          <style>.b1 {background: red; color: white; font-size: 15pt }</style>\
          </head>";
  str += F("<p><b>Доступные сети</b></p><body><form method=\"POST\" action=\"ok\">");
  //str += F("<input type=\"text\" name=\"ssid\"> WIFI SSID</br></br>");
  str += ssid_set;
  str += F("Текущий профиль WiFi: ");
  if(len_ssid < 32 && len_ssid != 0){
    str += ssid;
    str += F("</br></br>");
  }
  else    str += F("----------</br></br>");
  str += F("Пароль для выбранной сети : ");
  str += "</br></br>\
          <input type=\"text\" name=\"pswd\"> WiFi PASSWORD\
          </br></br>\
          Настройки  MQTT брокера :\
          </br></br>\
          <input type=\"text\" name=\"mqtt_server\">MQTT SERVER URL\
          </br></br>\
          <input type=\"text\" name=\"mqtt_port\">MQTT PORT\
          </br></br>\
          <input type=\"text\" name=\"mqtt_user\"> MQTT USER\
          </br></br>\
          <input type=\"text\" name=\"mqtt_pass\"> MQTT PASSWORD\
          </br></br>\
          <input type=SUBMIT value=\"Cохранить настройки\">\
          </form>\
          <form method=\"GET\" action=\"/upd\">\
          <input type=\"submit\" value=\"Обновить ПО\"> Текущая версия ПО: ";
  str +=  String(FW_VERSION);
   
  str += "</form>\
          <form method=\"GET\" action=\"/clear_wifi_setup\">\
          <input type=\"submit\" class=\"b1\" value=\"Очистить настройки !!!\">\
          </form>\
          <form method=\"GET\" action=\"/\">\
          <input type=\"submit\" value=\"Состояние теплицы\">\
          </form>\
          </body>\
          </html>";
  server.send ( 200, "text/html", str );
}





//==== ФУНКЦИЯ СОХРАНЕНИЯ НАСТРОЕК WiFi И MQTT ================================
void handleOk() {
  
  String mqtt_server_;
  String mqtt_user_;
  String mqtt_pass_;
  String mqtt_port_;

  String str = "";
  str += "<html>\
          <meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">\
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
            <a href=\"/reboot\">Перезагрузить</a></br>";
  } 
  else {
    str += "Cети WiFi не найдены !!!</br>\
          <a href=\"/wifi_setup\">Вернуться</a> на страницу настроек</br>";
  }
  str += "</body></html>";
  server.send ( 200, "text/html", str );
}



//-----------------------------------------------------------------------------
//                            ПЕРЕЗАГРУЗКА
//-----------------------------------------------------------------------------
void reboot() {
  handleRoot();
  delay(10);
  ESP.reset(); 
}

//-----------------------------------------------------------------------------
//                            WEB-СЕРВЕР
//-----------------------------------------------------------------------------
  void http_server() { 
    
    MDNS.begin(host);   
    server.on("/", handleRoot); 
    server.on("/setting", handleSet);
    server.on("/WiFi_setup", handleComm);
    server.on("/ok", handleOk);
    server.on("/ajustment", ajustment);
    server.on("/ctrl_mode", ctrl_mode);
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
    MDNS.addService("http", "tcp", 80);
    
    Serial.printf("HTTPUpdateServer ready! Open http://%s.local/update in your browser\n", host); 
}  

//-----------------------------------------------------------------------------
//                           MQTT CALLBACK
//-----------------------------------------------------------------------------
void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (int i=0;i<length;i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();
} 

//-----------------------------------------------------------------------------
//                           MQTT RECONNECT
//----------------------------------------------------------------------------- 
void reconnect() {
  if((millis() - event_mqtt_connect) > 10000){  // повторять через 10 секунд
    event_mqtt_connect = millis();
    //Serial.println();
    //Serial.print("SSID: ");
    //Serial.println(ssid);
    //Serial.print("Password: ");
    //Serial.println(pass);

    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());

    Serial.print(F("MQTT SERVER : "));
    Serial.println((String)mqtt_server);
  
    Serial.print(F("MQTT port : "));
    Serial.println(mqtt_port);
    
    Serial.print(F("MQTT user : "));
    Serial.println((String)mqtt_user);

    Serial.print(F("MQTT pass : "));
    Serial.println((String)mqtt_pass);
    Serial.print("Attempting MQTT connection...");
    if (client.connect("arduinoClient")) {
      Serial.println("connected");
      client.publish("outTopic","hello world");
      client.subscribe("inTopic");
    } 
    else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 10 seconds");
    }
  }
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
  
  //pinMode(tmr_rst, INPUT);  // вывод для рестарта таймера
  pinMode(tmr_opn, INPUT_PULLUP); // ожидаение импульса низкого уровеня
  pinMode(tmr_cls, INPUT_PULLUP); // ожидаение импульса низкого уровеня

  pinMode(ESP_BUILTIN_LED, OUTPUT);
  digitalWrite(ESP_BUILTIN_LED, LOW);

  // *** GPIO STATE
  //digitalWrite(tmr_rst, HIGH);
  digitalWrite(fan, LOW);
  digitalWrite(valve, LOW); // ... или восстановить состояние из EEPROM ?
  

  attachInterrupt(digitalPinToInterrupt(tmr_opn), ISR_tmr_opn, CHANGE);  // прерывание от таймера полива 
  attachInterrupt(digitalPinToInterrupt(tmr_cls), ISR_tmr_cls, CHANGE);  // прерывание от таймера полива 
    

  // *** EEPROM
  EEPROM.begin(512); // размер eeprom памяти 512 байт
  int eeprom = EEPROM.read(115);
  Serial.print("EEPROM.115 = ");
  Serial.println(eeprom);
  
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
  
  
  // *** WiFi точка доступа + WEB-сервер
  WiFi_AP();   
  http_server();  

   // *** WiFi станция - подключение к роутеру
  WiFi_STA();
  event_mqtt_connect = millis();

  // *** MQTT broker
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
  
  // *** инициализация опроса BME280 через I2C
  Wire.begin();
  event_bme_begin = millis();
  while(!bme.begin() && millis() - event_bme_begin < 5000);
  if (!bme.begin()) Serial.println("BME280 не найден !!!");
    
  

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


 
  Serial.println("Ready");  //  "Готово"
  Serial.print("IP address: ");  //  "IP-адрес: "
  Serial.println(WiFi.localIP()); 

  
  event_sensors_read = millis();
  event_serial_output = millis();
 

  
  
}

//-----------------------------------------------------------------------------
//  ОБРАБОТКА ПРЕРЫВАНИЯ ОТ ТАЙМЕРА ПОЛИВА - ЛОКАЛЬНАЯ КОМАНДА ОТКРЫТЬ КЛАПАН
//-----------------------------------------------------------------------------
void ISR_tmr_opn() {
  if (!ctrl && !digitalRead(valve) && !digitalRead(tmr_opn) && digitalRead(tmr_cls) && moist < set_moist) {
    local_valve = true;
    digitalWrite(valve,HIGH);
    Serial.println("ISR_tmr_opn = OPEN");
  }
  
}

//-----------------------------------------------------------------------------
//  ОБРАБОТКА ПРЕРЫВАНИЯ ОТ ТАЙМЕРА ПОЛИВА - ЛОКАЛЬНАЯ КОМАНДА ЗАКРЫТЬ КЛАПАН
//-----------------------------------------------------------------------------
void ISR_tmr_cls() {
  if (!ctrl && digitalRead(valve) && !digitalRead(tmr_cls) && digitalRead(tmr_opn)) {
    local_valve = false;
    digitalWrite(valve,LOW);
    Serial.println("ISR_tmr_cls = CLOSE");
  }  
}



//-----------------------------------------------------------------------------
//                                ОСНОВНОЙ ЦИКЛ
//-----------------------------------------------------------------------------
void loop() {

    
  
  server.handleClient();
  MDNS.update();

  digitalWrite(ESP_BUILTIN_LED, HIGH);
  
  // digitalWrite(ESP_BUILTIN_LED, LOW);
  //delay(500);
  //digitalWrite(ESP_BUILTIN_LED, HIGH);
  //delay(1000);

  // *** проверка подключения к МQTT брокеру и, при необходимости, переподключение
    if (WiFi.status() == WL_CONNECTED) if (!client.connected()) reconnect();
  
  // *** опрос датчиков и формирование локальных команд
    sensors_read();
  
  // *** включение реле вентилятора в локальном и дистанционном режимах
    boolean fan_cmd = (!ctrl && local_fan) || (ctrl && rem_fan);
    digitalWrite(fan, fan_cmd);

 
  // *** включение реле клапана полива в дистанционном режиме (в локальном режиме - по прерываниям)
    //if (ctrl) digitalWrite(valve, rem_valve);
    //if (!rem_valve) Serial.println("Реле клапана полива. Дист команда = ВЫКЛ.");  
    //if (rem_valve) Serial.println("Реле клапана полива. Дист.команда = ВКЛ.");
    
  // *** вывод информации в порт    
    if (millis() - event_serial_output > 10000) { // повторять через 10 секунд
      event_serial_output = millis();
      if (!ctrl) Serial.println("ctrl = false");  
      if (ctrl) Serial.println("ctrl = true");
      if (!digitalRead(fan)) Serial.println("Реле вентилятора = ВЫКЛ.");  
      if (digitalRead(fan)) Serial.println("Реле вентилятора = ВКЛ."); 
      if (!digitalRead(valve)) Serial.println("Реле клапана воды = ВЫКЛ.");  
      if (digitalRead(valve)) Serial.println("Реле клапана воды = ВКЛ."); 
    }
  
    
  
  client.loop();  


  
  yield(); 
}
