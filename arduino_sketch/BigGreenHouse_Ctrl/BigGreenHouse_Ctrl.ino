/* ------------------------------------------------------------
                      BIG GREENHOUSE CTRL
   ------------------------------------------------------------

BME280 I2C (Температура воздуха, Давление, Влажность воздуха)
     Vin (Voltage In)     ->  3.3V
     Gnd (Ground)         ->  Gnd
     SDA (Serial Data)    ->  JPIO4 (D2) on D1 Mini
     SCK (Serial Clock)   ->  JPIO5 (D1) on D1 Mini
	 
CAPACITIVE SOIL MOISTURE SENSOR (Влажность почвы)	 
	 Vin (Voltage In)     ->  3.3V
     Gnd (Ground)         ->  Gnd
     AI  (Analog Input)   ->  ADC0 (A0) on D1 Mini 

PULS FLOW SENSOR (Расход воды на полив)
	 Vin (Voltage In)     ->  3.3V
     Gnd (Ground)         ->  Gnd
     DI  (Digital Input)  ->  JPIO13 (D7) on D1 Mini	 

OUT VALVE RELAY (Орошение)
     DO  (Digital Output) ->  JPIO16 (D0) on D1 Mini	 

FAN RELAY (Вентиляция)
     DO  (Digital Output) ->  JPIO14 (D5) on D1 Mini	

IN VALVE RELAY (Наполнение бака с водой - Резерв)
     DO  (Digital Output) ->  JPIO12 (D6) on D1 Mini
	 
	 
*/

// **** ИСПОЛЬЗУЕМЫЕ БИБЛИОТЕКИ 
#include <ESP8266WiFi.h>					                // библиотека ESP8266WiFi
#include <PubSubClient.h>					                // библиотека PubSubClient (MQTT)
#include <ESP8266WebServer.h>				              // библиотека ESP8266WebServer
#include <BME280I2C.h>              		          // библиотека ВМЕ280I2C
// #include <OLED_I2C.h>             		          // библиотека OLED_I2C

// **** НАСТРОЙКИ ПОДКЛЮЧЕНИЯ 
const char* ssid     	    = "******";             // Название точки доступа     
const char* password 	    = "******";             // Пароль                     
const char* MQTT_server   = "192.168.123.222";
String topic              = "BigGreenHouse";         
#define MQTT_user 		"username"
#define MQTT_password "password"


WiFiClient espClient;
PubSubClient client(espClient);




     
// **** НАСТРОЙКИ BME280
// bme(<Temperature Oversampling Rate>, <Humidity Oversampling Rate>, <Pressure Oversampling Rate>, <Mode>, <Standby Time>, <Filter>, <SPI Enable>, <BME280 Address>)
// BME280I2C bme(0x1, 0x1, 0x1, 0x1, 0x5, 0x0, false, 0x77); 

BME280I2C bme;								// По умолчанию

  
// **** ОБЪЯВЛЕНИЕ ПЕРЕМЕННЫХ

unsigned long PreTime;                                                              // отметка системного времени в предыдущем цикле
unsigned long LastMsg = 0;
unsigned long Last;
long Cycle;                                                                         // мс, длительность цикла

float temp, hum, pres, moist;														// значения датчиков
//float temp(NAN), hum(NAN), pres(NAN);

float set_temp, set_hum, set_moist;

int year, month, day_m, day_w, hour, minute, second, ms;							// переменные для учета времени
  

int flow_meter 			  = 13;  
int ventilation_fan 	= 14;
int irrigation_valve 	= 16;



// ------------------------------------------------------------
//          SETUP_WiFi - ПРОЦЕДУРА ПОДКЛЮЧЕНИЯ К WiFi
// ------------------------------------------------------------
void setup_wifi() {
  delay(10);
  Serial.print("Connecting to ");					                              // сообщение "Connecting to "
  Serial.println(ssid);								                                  // <имя WiFi сети>
  WiFi.mode(WIFI_STA);								                                  // старт подключения к WiFi
  WiFi.begin(ssid, password);						                                // с ранее определенными <ssid> и <password>
  while (WiFi.status() != WL_CONNECTED) {			                          // до тех пор, пока нет WiFi подключения
    delay(500);
    Serial.print(".");								                                  // сообщение "..... "
  }
  Serial.println("");								                                    // после установления соединения с WiFi
  Serial.println("WiFi connected");					                            // сообщение "WiFi connected" 
  Serial.print("IP address: ");						                              // сообщение "IP address: "
  Serial.println(WiFi.localIP());					                              // <WiFi.localIP>
}


// ------------------------------------------------------------
//           RECONNECT - ПРОЦЕДУРА ПОДКЛЮЧЕНИЯ К MQTT
// ------------------------------------------------------------
void reconnect() {
  while (!client.connected()) {						                              // до тех пор, пока нет подключения к MQTT
    Serial.print("Attempting MQTT connection...");	                    // сообщение "Attempting MQTT connection..."                   
    char clientId[] = "BigGreenHouse";                                  // ID MQTT-клиента 
//  if (client.connect(clientId,MQTT_user,MQTT_password)) {    
    if (client.connect(clientId)){                                      // если клиент подключился к брокеру,  
      Serial.println("connected");                                      // сообщение "connected"
      String reseiver = topic + "/cmd/#";                               
      client.subscribe(reseiver.c_str());                               // подписка на избранные топики
      client.subscribe((topic + "/cmd/#").c_str());
    } 
    else {										                                          // иначе,
      Serial.print("failed, rc=");					                            // сообщение "failed, rc="
      Serial.print(client.state());					                            // <код состояния>
      Serial.println(" try again in 5 seconds");	                      // сообщение " try again in 5 seconds"
      delay(5000);									                                    // пауза перед повторным соединением 
    }
  }
}

// ------------------------------------------------------------
//        CALLBACK_MQTT - ПРОЦЕДУРА ПОЛУЧЕНИЯ СООБЩЕНИЙ
// ------------------------------------------------------------
void callback_mqtt(char* topic, byte *payload, unsigned int length) {
    Serial.println("New message from broker");
    Serial.print("channel:");
    Serial.println(topic);
    Serial.print("data:");  
    Serial.write(payload, length);
    Serial.println();
}



// ------------------------------------------------------------
//                 SETUP - Инициализация
// ------------------------------------------------------------

void setup() {                  
  PreTime = millis();                                                               // сохранить системное время 

  // **** ИНИЦИАЛИЗАЦИЯ ВЫВОДОВ
  pinMode(LED_BUILTIN, OUTPUT);				            // встроенный светодиод
  digitalWrite(LED_BUILTIN, HIGH);			          // зажечь
  delay(100);								                      // на 100 ms
  digitalWrite(LED_BUILTIN, LOW);			            // погасить  
  

  pinMode(flow_meter, INPUT);				              // подключение расходомера

  pinMode(12, OUTPUT);						                // подключение реле клапана наполнения бака
  pinMode(ventilation_fan, OUTPUT);			          // подключение реле вентиляции
  pinMode(irrigation_valve, OUTPUT);  		        // подключение реле клапана полива
 
  
// **** SERIAL ПОРТ
  Serial.begin(9600);						 

// **** WiFi
  setup_wifi();
  
// **** MQTT 
  client.setServer(MQTT_server, 1883);
  reconnect();
  
// **** BME280  
  while (!bme.begin()) {            		        // до тех пор, пока BME280 не обнаружен   
    Serial.println ("bme280 not found");        // сообщение "bme280 not found"               
    digitalWrite(LED_BUILTIN, HIGH);            
    delay(500);								                  // мигать
    digitalWrite(LED_BUILTIN, LOW);             // встроенным светодиодом
    delay(500);								                  // с частотой 1 Hz
  }

  delay(500);  
  PreTime = millis();               		        // отметка времени в конце цикла программы 
}

// ------------------------------------------------------------
//                 LOOP - Основной цикл
// ------------------------------------------------------------

void loop() {                   

  if (!client.connected()) {				            // если нет подключения к MQTT брокеру, 
    reconnect();							                  // переподключение и подписка на топики
  } 
  client.loop();							                  // MQTT клиент ...
  long NowTime = millis();
  if (NowTime - LastMsg > 10000) {			        // каждые 6 s (10 раз в минуту)
    LastMsg = NowTime;
    sensors_rd();							                  // опрос датчиков

    String MsgVent;
    
/* ВКЛЮЧИТЬ ВЕНТИЛЯТОР, если
   температура воздуха превышает заданное значение на 2.5 °C
   или влажность воздуха превышает заданное значение на 2.5 %.
*/
    if ((temp > set_temp + 2.5) || (hum > set_hum + 2.5)) {
	  digitalWrite (ventilation_fan, HIGH);
	  MsgVent = "ON";
    char vent[] = "ON";
	  }
	
/* ВЫКЛЮЧИТЬ ВЕНТИЛЯТОР, если
   температура воздуха ниже заданного значения на 2.5 °C
   и влажность воздуха ниже заданного значения на 2.5 %
   или если температура воздуха ниже заданного значения на 5.0 °C
*/
    if (((temp < set_temp - 2.5) && (hum < set_hum - 2.5)) || (temp < set_temp - 5.0)) {
	  digitalWrite (ventilation_fan, LOW);
	  MsgVent = "OFF";
    char vent[] = "OFF";
	  }

    String MsgIrrig;
    
/* ВКЛЮЧИТЬ КЛАПАН ПОЛИВА, если
   температура воздуха превышает заданное значение на 2.5 °C
   или влажность воздуха превышает заданное значение на 2.5 %.
*/ 	

// **** ПОДГОТОВКА ДАННЫХ И ПУБЛИКАЦИЯ

    String msg="";
    char MsgTemp[10];
    char MsgHum[10]; 
	  char MsgPres[10];
    char MsgMoist[10];
    char MsgFlow[10];
	
//	msg = temp; 
//	msg = msg + "°C";
//	msg.toCharArray(MsgTemp,10);
    dtostrf(temp, 2, 3, MsgTemp);
	
//	msg = hum; 
//	msg = msg + "%";
//	msg.toCharArray(MsgHum,10);	
    dtostrf(hum, 2, 3, MsgHum);
	
//	msg = pres; 
//	msg = msg + "mmHg";
//	msg.toCharArray(MsgPres,10);
    dtostrf(pres, 2, 3, MsgPres);
	
//	msg = moist; 
//	msg = msg + "%";
//	msg.toCharArray(MsgMoist,10);
    dtostrf(moist, 2, 3, MsgMoist);
	
    client.publish ("BigGreenHouse/info/Temperature", MsgTemp);
    client.publish ("BigGreenHouse/info/Temperature", String(temp).c_str(),true);
    client.publish ("BigGreenHouse/info/Humidity", MsgHum);
    client.publish ("BigGreenHouse/info/Pressure", MsgPres);
    client.publish ("BigGreenHouse/info/Soil Moisture", MsgMoist);
	  client.publish ("BigGreenHouse/info/Water Flow", MsgFlow);
    client.publish ("BigGreenHouse/info/Ventilation", MsgVent.c_str(),true);
    client.publish ("BigGreenHouse/info/Irrigation", MsgIrrig.c_str(),true);
  }

// **** СИСТЕМНОЕ ВРЕМЯ  
  Cycle = NowTime - PreTime;                                                        // прошло времени с предыдущего цикла программы 
  ms = ms + Cycle;
  if (ms >= 1000) {
	ms -= 1000;
	second += 1;
  }
  if (second >= 60) {
    second -= 60;
    minute += 1;
  }
  if (minute >=60) {
    minute -=60;
	  hour += 1;
  }
  if (hour >= 24) {
    hour -= 24;
	  day_m += 1;
    day_w += 1;
  }
  if (day_w >= 8) {
    day_w = 1;
  }
  if ((day_m >= 28) && (month == 2) && (year % 4 != 0)) {
    day_m -= 28;
	  month = 3;
  }
  if ((day_m >= 29) && (month == 2) && (year % 4 == 0)) {
    day_m -= 29;
	  month = 3;
  }
  if ((day_m >= 30) && ((month == 4) || (month == 6) || (month == 9) || (month == 11))) {
    day_m -= 30;
	  month += 1;
  }
  if ((day_m >= 31) && ((month == 1) || (month == 3) || (month == 5) || (month == 7) || (month == 8) || (month == 10))) {
    day_m -= 31;
	  month += 1;
  }
  if ((day_m >= 31) && (month == 12))  {
    day_m -= 31;
	  month = 1;
	  year += 1;
  }
  PreTime = NowTime; 
  
  
}


// ------------------------------------------------------------
//                 SENSORS_RD - Опрос датчиков
// ------------------------------------------------------------
void sensors_rd() {

// **** ОПРОС ДАТЧИКА BME280
  float temp(NAN), hum(NAN), pres(NAN);
//  bool metric = true;
//  uint8_t pressureUnit(0);   						// B000 = Pa, B001 = hPa, B010 = Hg, B011 = atm, B100 = bar, B101 = torr, B110 = N/m^2, B111 = psi   
  BME280::TempUnit tempUnit(BME280::TempUnit_Celsius);
  BME280::PresUnit presUnit(BME280::PresUnit_Pa);
  bme.read(pres, temp, hum, tempUnit, presUnit);  // считать значения давления, температуры, влажности, в метрической системе, давление в Ра 
//  temp -= 0.3;                  					// correct temp
  pres /= 133.3;                  					// convert to mmHg
  
// **** ОПРОС ДАТЧИКА ВЛАЖНОСТИ ПОЧВЫ  
  const int air_val = 520;							// показания датчика на воздухе, соответствуют 0%
  const int water_val = 260;  						// показания датчика в воде, соответствуют 100%
  int sensor_moist = analogRead(0);
  
  if (sensor_moist <= air_val) {
	sensor_moist = air_val;
  }
  if (sensor_moist >= water_val) {
	sensor_moist = water_val;
  }
  moist = 100.0 * float(air_val-sensor_moist) / float(air_val-water_val);
  
// *** ВЫВОД ИНФОРМАЦИИ С ДАТЧИКОВ
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
  Serial.print (sensor_moist);  
  Serial.println ("]"); 
}
