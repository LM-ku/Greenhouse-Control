#define FW_VERSION "FW v.1.0. debag"


// === Library === 
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <WiFiUdp.h> 
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <EEPROM.h>
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
String sta_ssid = "";
String sta_pass = "";

// === MQTT ===
//IPAddress mqtt_server(192, 168, 123, 222);
char* mqtt_server = "192.168.123.222";
int mqtt_port = 1883;
String buf_mqtt_port = "";
String mqtt_user = "";
String mqtt_pass = "";

byte step_mqtt_send = 0; // переменная для поочередной (по однаму сообщению за цикл loop) публикации сообщений на MQTT брокере
String id_messages_ = "";
boolean send_ = 0;

WiFiClient espClient;
PubSubClient client(espClient); 


// === WEB SERVER ===
ESP8266WebServer server(80);
byte len_ssid, len_pass, len_mqtt_server, len_mqtt_port, len_mqtt_user, len_mqtt_pass;
const char* host = "OTA";
String ssid = "";
String pass = "";


boolean ctrl = false; // режим: false = local, true = remote
boolean rem_valve = false;  // дистанционная команда для реле клапана полива: false = выключить, true = включить
boolean rem_fan = false;  // дистанционная команда для реле вентилятора: false = выключить, true = включить
boolean local_valve = false;  // локальная команда для реле клапана полива: false = выключить, true = включить
boolean local_fan = false;  // локальная команда для реле вентилятора: false = выключить, true = включить


// === BME280 ===
float temp;
float pres;
float hum;
float moist;
float set_temp;
float set_hum;
float set_moist;




















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





//-----------------------------------------------------------------------------
//                    ФУНКЦИЯ ПРЕБРАЗОВАНИЯ FLOAT В STRING 
//-----------------------------------------------------------------------------
String float_to_String(float n){
  String ansver = "";
  ansver = String(n);
  String ansver1 = ansver.substring(0, ansver.indexOf('.'));//отделяем число до точки
  int accuracy;
  accuracy = ansver.length() - ansver.indexOf('.');//определяем точность (сколько знаков после точки)
  int stepen = 10;
  for(int i = 0; i < accuracy; i++) stepen = stepen * 10;  
  String ansver2 = String(n * stepen);// приводит float к целому значению с потеряй разделителя точки(в общем удаляет точку)
  String ansver3 = "";
  ansver3 += ansver1 += '.';
  ansver3 += ansver2.substring((ansver.length() - accuracy), (ansver.length() - 1));
  ansver3 += '\n';
  return(ansver3);   
}


//-----------------------------------------------------------------------------
//               ФУНКЦИЯ ЗАПИСИ FLOAT ЗНАЧЕНИЯ В EEPROM
//-----------------------------------------------------------------------------
void EEPROM_float_write(int addr, float val)  { // запись Float в ЕЕПРОМ
  byte *x = (byte *)&val;
  for(byte i = 0; i < 4; i++) EEPROM.put(i+addr, x[i]);
}



//-----------------------------------------------------------------------------
//               ФУНКЦИЯ ЧТЕНИЯ FLOAT ЗНАЧЕНИЯ ИЗ EEPROM
//-----------------------------------------------------------------------------
float EEPROM_float_read(int addr) { // чтение Float из ЕЕПРОМ
  byte x[4];
  for(byte i = 0; i < 4; i++) x[i] = EEPROM.read(i+addr);
  float *y = (float *)&x;
  return y[0];
}





//-----------------------------------------------------------------------------
//                       ФУНКЦИЯ "ТОЧКА ДОСТУПА" 
//-----------------------------------------------------------------------------
void WiFi_AP() {
  Serial.print("Setting soft-AP configuration ... ");
  Serial.println(WiFi.softAPConfig (local_IP, gateway, subnet) ? "Ready" : "Failed!");
  Serial.print("Setting soft-AP ... ");
  Serial.println(WiFi.softAP(ap_ssid, ap_pass) ? "Ready" : "Failed!");
  Serial.print("Soft-AP IP address = ");
  Serial.println(WiFi.softAPIP());
  WiFi.printDiag(Serial);
}

//-----------------------------------------------------------------------------
//                      ФУНКЦИЯ "СКАНИРОВАНИЕ WiFi" 
//-----------------------------------------------------------------------------
void WiFi_Scan() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
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
}




//-----------------------------------------------------------------------------  
//         ФУНКЦИЯ НАСТРОЕК УПРАВЛЕНИЯ ТЕПЛИЦЕЙ ЧЕРЕЗ WEB-СЕРВЕР 
//-----------------------------------------------------------------------------
void GreenHouse_control(){
  String str = "";
  str += F("<html>\
            <head><meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">\
            <title>НАСТРОЙКИ УПРАВЛЕНИЯ ТЕПЛИЦЕЙ</title>\
            <style>body { background-color: #FFFFFF; font-family: Arial, Helvetica, Sans-Serif; Color: #000000; font-size: 10pt}</style>\
            </head>\
            <body>\
            <p>Время и дата на момент обновления страницы</p><br /><hr /><p>");    
  str += String(curent_time);
  str += F("<p>Атмосферное давление .. ");
  str += String(pres);
  str += F(" mm.Hg </p>\
            <p>Температура воздуха ...... ");
  str += String(temp);
  str += F(" град.С </p>\
            <p>Влажность воздуха .......... ");
  str += String(hum);
  str += F(" % </p><br />");
  //if(digitalRead(14) == HIGH && mode > 0) {
    str += F("<p><font color = \"#FF0000\" size=\"2\"># ВЕНТИЛЯЦИЯ ВКЛЮЧЕНА!</font> </p>");
  //}
  //if(digitalRead(14) == LOW && mode == 2) {
    //str += F("<p><font color = \"#0000FF\" size=\"2\"># ВЕНТИЛЯЦИЯ ВЫКЛЮЧЕНА!</font> </p>");
  //}
  str += F("<br /><p>Влажность почвы ............. ");
  str += String(moist);
  str += F(" % </p><br />");
  //if(digitalRead(14) == HIGH && mode > 0) {
  //  str += F("<p><font color = \"#FF0000\" size=\"2\"># ИДЕТ ПОЛИВ!</font> </p>");
  //}
  //if(digitalRead(14) == LOW && mode == 2) {
    str += F("<p><font color = \"#0000FF\" size=\"2\"># ПОЛИВ ВЫКЛЮЧЕН!</font> </p>");
  //}
  str += F("<p>после предыдущего полива прошло </p>\
            <p>дней : ");
  //str += String(last_irrig_day);
  str += F(" часов : ");
  //str += String(last_irrig_hour);
  str += F("</p><br /><hr /><p><font size=\"1\"><b>РЕЖИМ УПРАВЛЕНИЯ</b></font></p>\
            <form action=\"/ctrl_mode\" method=\"POST\" >\
            <input type=\"radio\" name=\"ctrl\" value=\"false\" ");
  if(!ctrl)str += F("checked");
  str += F("/> Локальный\
            <input type=\"radio\" name=\"ctrl\" value=\"true\" ");
  if(ctrl)str += F("checked");
  str += F("/> Дистанционный<br /><br />\
            <input type=\"submit\" value=\"Установить режим\" />\
            </form>\
            </p><br /><hr /><p><font size=\"1\"><b>УСТАВКИ УПРАВЛЕНИЯ</b></font></p>\
            <form action=\"/setting\" method=\"POST\">\
            <input type=\"text\" style=\"width: 50\" name=\"set_temp\" value=\"");
  //str += float_to_String(set_temp);
  str += F("\n\"/> уставка температуры воздуха<br /><br />\
            <input type=\"text\" style=\"width: 50\" name=\"set_hum\" value=\"");
  //str += float_to_String(set_hum);
  str += F("\n\"/> уставка влажности воздуха<br /><br />\
            <input type=\"text\" style=\"width: 50\" name=\"set_moist\" value=\"");
  //str += float_to_String(set_moist);
  str += F("\n\"/> уставка влажности почвы<br /><br />\
            <input type=\"submit\" value=\"Сохранить уставки\" />\
            </form>\
            <hr /><br /><br />\
            <form action=\"/WiFi_setup\" method=\"GET\">\
            <input type=\"submit\" value=\"WiFi настройки\" />\
            </form><br /><br />\
            </body>\
            </html>\n\r");
  server.send ( 200, "text/html", str );   
}

//------------------------------------------------------------------------------
//               ФУНКЦИЯ ИЗМЕНЕНИЯ НАСТРОЕК СЕТИ WiFi И MQTT БРОКЕРА
//------------------------------------------------------------------------------
void handleRoot() {
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
  str += "</br></br><input type=\"text\" name=\"pswd\"> WiFi PASSWORD</br></br>Настройки  MQTT брокера : </br></br><input type=\"text\" name=\"mqtt_server\"> MQTT SERVER URL</br></br><input type=\"text\" name=\"mqtt_port\"> MQTT PORT</br></br><input type=\"text\" name=\"mqtt_user\"> MQTT USER</br></br><input type=\"text\" name=\"mqtt_pass\"> MQTT PASSWORD</br></br><input type=SUBMIT value=\"Cохранить настройки\"></form><form method=\"GET\" action=\"/update\"><input type=SUBMIT value=\"Обновить ПО\"> Текущая версия ПО: ";
  str +=  String(FW_VERSION);
   
  str += "</form><form method=\"GET\" action=\"/clear_wifi_setup\"><input type=SUBMIT class=\"b1\" value=\"Очистить настройки !!!\"></form><form method=\"GET\" action=\"/\"><input type=SUBMIT value=\"Настройки управления теплицей\"></form></body></html>";
  server.send ( 200, "text/html", str );
}
//=============================================================================

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

  sta_ssid = server.arg(0);
  sta_pass = server.arg(1);
  
  mqtt_server_ = server.arg(2);
  mqtt_port_ = server.arg(3);
  mqtt_user_ = server.arg(4);
  mqtt_pass_ = server.arg(5);

  if (sta_ssid != 0) {
    //EEPROM.put(96, sta_ssid.length()); 
    //EEPROM.put(97, sta_pass.length());
    Serial.print("EEPROM.96 <--");   
    Serial.println(sta_ssid.length());
    Serial.print("EEPROM.97 <--");   
    Serial.println(sta_pass.length());
    
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
       
    for (byte i = 0; i < sta_ssid.length(); i++) {
      //EEPROM.put(i, sta_ssid.charAt(i));
      Serial.print("EEPROM.");
      Serial.print(i);
      Serial.print(" <-- "); 
      Serial.println(sta_ssid.charAt(i)); 
    }
    
    for (byte i = 0; i < sta_pass.length(); i++)  {
      EEPROM.put(i + 32, sta_pass.charAt(i));
      Serial.print("EEPROM."); 
      Serial.print(i+32);
      Serial.print(" <-- ");  
      Serial.println(sta_pass.charAt(i)); 
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
//=============================================================================
  
//==== ФУНКЦИЯ WEB-СЕРВЕРА ====================================================
  void http_server() {
    
    server.on("/", GreenHouse_control); 
    server.on("/WiFi_setup", handleRoot);
    //server.on("/reboot", resetFunc);
    //server.on("/setting", setting);
    server.on("/ok", handleOk);
    //server.on("/clear_WiFi_setup", clear_wifi_setup);
    MDNS.begin(host);
    //httpUpdater.setup(&server);
    server.begin();
  
    MDNS.addService("http", "tcp", 80);
    Serial.printf("HTTPUpdateServer ready! Open http://%s.local/update in your browser\n", host);
  
    delay(100);
    Serial.println(F("HTTP server started"));
    delay(20);
    
}  
//=============================================================================



//==== СТАРТОВАЯ ИНИЦИАЛИЗАЦИЯ ================================================
void setup() {
  delay(1000);
  Serial.begin(115200);
  delay(10);
   
  EEPROM.begin(512); // размер eeprom памяти 512 байт
  int eeprom = EEPROM.read(115);
  Serial.print("EEPROM.115 = ");
  Serial.println(eeprom);
  if(eeprom == 170)  {  //если в eeprom достоверные данные
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

    // восстанавливаем url mqtt сервера, но только если длинна len_mqtt_server менее 32 символа  
    mqtt_server = "";
    if(len_mqtt_server <= 32){  
      for(byte i = 0; i <= len_mqtt_server; i++)mqtt_server += char(EEPROM.read(404 + i));
    }
  
    // восстанавливаем mqqt порт, но только если длинна менее 6 символов   
    String mqtt_port_ = "";
    if(len_mqtt_port < 6){  
      for(byte i = 0; i < len_mqtt_port; i++)mqtt_port_ += char(EEPROM.read(436 + i));    
    }
    mqtt_port = mqtt_port_.toInt();
  
    // восстанавливаем mqqt user    
    if(len_mqtt_user <= 16){
    for(byte i = 0; i <= len_mqtt_user; i++)mqtt_user += char(EEPROM.read(441 + i));
    delay(10);
    }

    // восстанавливаем mqqt pass     
    if(len_mqtt_pass <= 32){
    for(byte i = 0; i <= len_mqtt_pass; i++)mqtt_pass += char(EEPROM.read(457 + i));
    delay(10);
    }  
  }
  else  {
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
  
  }
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
   



  
  
 
  
  //server.on("/", handleRoot);
  //server.begin();
  
  //Serial.println("HTTP server started");
}
//=============================================================================


//==== ОСНОВНОЙ ЦИКЛ ==========================================================
void loop() {



  
  server.handleClient();
}
//=============================================================================
