/* ------------------------------------------------------------
                      BIG GREENHOUSE CTRL
   ------------------------------------------------------------

BME280 I2C (Температура воздуха, Давление, Влажность воздуха)
     Vin (Voltage In)     ->  3.3V
     Gnd (Ground)         ->  Gnd
     SDA (Serial Data)    ->  JPIO4 
     SCK (Serial Clock)   ->  JPIO5 
	 
CAPACITIVE SOIL MOISTURE SENSOR (Влажность почвы)	 
     Vin (Voltage In)     ->  3.3V
     Gnd (Ground)         ->  Gnd
     AI  (Analog Input)   ->  ADC0 

VALVE RELAY (Орошение)
     DO  (Digital Output) ->  JPIO13 	 

FAN RELAY (Вентиляция)
     DO  (Digital Output) ->  JPIO15	
     
IRRIGATION TIMER
     DI  (Digital Input)  ->  JPIO12  puls for START irrigation 
     DI  (Digital Input)  ->  JPIO14  puls for STOP irrigation
     DO  (Digital Output) ->  JPIOxx  puls for synchronization
	 
*/

// **** WiFi 
#include <ESP8266WiFi.h>					      	// библиотека ESP8266WiFi
const char* ssid  	= "******";             			// Название точки доступа     
const char* password	= "******";             			// Пароль     


// **** MQTT
#include <PubSubClient.h>					      	// библиотека PubSubClient (MQTT)
const char *mqtt_server = "192.168.123.222"; 
const int mqtt_port 	= 1886; 
const char *mqtt_user 	= "Login"; 
const char *mqtt_pass 	= "Pass"; 
String topic            = "BigGreenHouse"; 
WiFiClient espClient;
PubSubClient client(espClient);


#define MQTT_user 	"username"
#define MQTT_password 	"password"

// **** WEB cервер 
#include <ESP8266WebServer.h>				              	// библиотека ESP8266WebServer


// **** BME280
#include <BME280I2C.h>              		          		// библиотека ВМЕ280I2C
// bme(<Temperature Oversampling Rate>, <Humidity Oversampling Rate>, <Pressure Oversampling Rate>, <Mode>, <Standby Time>, <Filter>, <SPI Enable>, <BME280 Address>)
// BME280I2C bme(0x1, 0x1, 0x1, 0x1, 0x5, 0x0, false, 0x77); 
BME280I2C bme;								
float temp, hum, pres, moist;


// **** ОБЪЯВЛЕНИЕ ПЕРЕМЕННЫХ
float set_temp, set_hum, set_moist;	// уставки температуры и влажности воздуха, влажности почвы
int air_val, water_val;	// пороговые значения датчика влажности почвы
int last_irr_month, last_irr_day, last_irr_hour;	// месяц, день и час окончания предыдущего полива

int relay_valve = 13;	// gpio 13 - управление реле клапана воды               
int relay_fan 	= 15;	// gpio 15 - управление реле вентиляторов
int timer_start	= 16;	// gpio 16 - инпульс --> HIGH таймера при наступлении события  
int timer_stop	= 14;	// gpio 14 - импульс --> HIGH таймера при завершении заданного интервала  
int timer_rst	= 12;  	// gpio 12 - двойной импульс HIGH --> LOW для инициализации таймера 
//				     при дистанционном включении реле клапана воды
//				     с этого момента начнется новый отсчет времени таймером
//				     для начала следующего полива 

boolean local_fan, local_valve, rem_fan, rem_valve;

byte state;	// bit 0 - irrigation 
		// bit_1 - ... 
		// bit_2 - ... 
		// bit_3 - ... 
		// bit_4 - ... 
		// bit_5 - ... 
		// bit_6 - ... 
		// bit_7 - ... 

float set_temp 	= 22.0;
float set_hum	= 50.0;


unsigned long PreTime;                                           	// отметка системного времени в предыдущем цикле
unsigned long LastMsg = 0;
unsigned long Last;
long Cycle;                                                             // мс, длительность цикла

char msg[50];
int value = 0;									




// ------------------------------------------------------------
//                 SENSORS_RD - ОПРОС ДАТЧИКОВ
// ------------------------------------------------------------
void sensors_rd() {

// **** ОПРОС ДАТЧИКА BME280
  //float temp(NAN), hum(NAN), pres(NAN);
  	
  BME280::TempUnit tempUnit(BME280::TempUnit_Celsius);
  BME280::PresUnit presUnit(BME280::PresUnit_Pa);
  bme.read(pres, temp, hum, tempUnit, presUnit);  // считать значения давления, температуры, влажности, в метрической системе, давление в Ра 
  //temp -= 0.3;	// correct temp
  pres /= 133.3;	// convert to mmHg
  
// **** ОПРОС ДАТЧИКА ВЛАЖНОСТИ ПОЧВЫ  
  air_val = 520;	// EEPROM.read(xxx, air_val) - показания датчика на воздухе, соответствуют 0% влажности
  water_val = 260;	// EEPROM.read(xxx, water_val) - показания датчика в воде, соответствуют 100% влажности
    	
	
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
  

// ------------------------------------------------------------
//              OUTPUT CTRL - УПРАВЛЕНИЕ ВЫХОДАМИ
// ------------------------------------------------------------
// В ЛОКАЛЬНОМ РЕЖИМЕ (ctrl == false) :
// - реле вентиляции управляется по разности текущей и заданной температуры и влажности воздуха,
// - реле клапана воды включается по сигналу от таймера, отключается в зависимости от влажности почвы,
// уставки температуры и влажности изменяются через Web-сервер или через получение соответствующих 
// сообщений от MQTT брокера.
// В ДИСТАНЦИОННОМ РЕЖИМЕ (ctrl == true) :
// - реле вентиляции включается и выключается командами от MQTT брокера,
// - реле клапана воды включается и выключается командами от MQTT брокера,
// - при включении клапана воды формируются два импульса (HiGH --> LOW) для инициализации начала отсчета
//   времени между включениями клапана воды.
void output_ctrl() {
  set_temp = 22.5; 	// EEPROM_read_float
  set_hum = 50.0;	// EEPROM_read_float
  // - вентиляция
  if (temp > set_temp+2.5) local_cmd_fan = true ;	// если жарко, включить вентиляцию
  if (hum > set_hum+10.0) local_cmd_fan = true ;	// если влажно, включить вентиляцию
  if (temp < set_temp-2.5) && (hum < set_hum-10.0) local_cmd_fan = false ;	// если не жарко и не влажно, выключить вентиляцию
  if (temp < 15.0) local_fan = false ; 	// если температура ниже 15 градусов, выключить вентиляцию
  // - ирригация
  if (digital.Read(timer_start)&&!digital.Read(timer_stop)) local_cmd_valve = true;
  if (!digital.Read(timer_start)&&digital.Read(timer_stop)) local_cmd_valve = false;  
  Write(state, 0, local_valve);
  state[0] = local_cmd_valve;
  //EEPROM.put(xxx,state);	// сохранить состояние в eeprom
	

}


// ------------------------------------------------------------
//          SETUP_WiFi - ПРОЦЕДУРА ПОДКЛЮЧЕНИЯ К WiFi
// ------------------------------------------------------------
void setup_wifi() {
  int ct = 0;	
  delay(10);
  Serial.print("Connecting to ");					
  Serial.println(ssid);							
  WiFi.mode(WIFI_STA);	
  WiFi.begin(ssid, password);		
  while (WiFi.status() != WL_CONNECTED && ct < 5) {			// не более 5 попыток подключения к WiFi
    delay(500);
    Serial.print(".");						       	// сообщение "..... "
    randomSeed(micros());
    ct += 1;	  
  }
  Serial.println("");	
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected");					// сообщение "WiFi connected" 
    Serial.print("IP address: ");					// сообщение "IP address: "
    Serial.println(WiFi.localIP());					// <WiFi.localIP>
  }
  else {
    Serial.println("WiFi NOT connected");				// сообщение "WiFi NOT connected" 
  }
}

// ------------------------------------------------------------
//           RECONNECT - ПРОЦЕДУРА ПОДКЛЮЧЕНИЯ К MQTT
// ------------------------------------------------------------
void reconnect() {
  int ct = 0;	
  while (!client.connected() && ct < 10) {				// не более 10 попыток подключения к брокеру		                              // до тех пор, пока нет подключения к MQTT
    Serial.print("Attempting MQTT connection...");	                // сообщение "Attempting MQTT connection..."                   
    char clientId[] = "BigGreenHouse";                                  // ID MQTT-клиента 
//  if (client.connect(clientId,MQTT_user,MQTT_password)) {    
    if (client.connect(clientId)){                                      // если клиент подключился к брокеру,  
      Serial.println("connected");                                      // сообщение "connected"
      client.subscribe((topic + "/cmd/#").c_str());			// подписка на избранные топики
    } 
    else {										                                          // иначе,
      Serial.print("failed, rc=");					// сообщение "failed, rc="
      Serial.print(client.state());					// <код состояния>
      Serial.println(" try again in 5 seconds");	                // сообщение " try again in 5 seconds"
      delay(5000);							// пауза перед повторным соединением 
    }
    ct += 1;
  }
}

// ------------------------------------------------------------
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


// ------------------------------------------------------------
//                 SETUP - Инициализация
// ------------------------------------------------------------

void setup() {                  
  PreTime = millis();                                              	// сохранить системное время 

// **** ИНИЦИАЛИЗАЦИЯ ВЫВОДОВ
  pinMode(LED_BUILTIN, OUTPUT);	// встроенный светодиод
  pinMode(relay_fan, OUTPUT);	// подключение реле вентиляции
  pinMode(relay_valve, OUTPUT);	// подключение реле клапана полива
 
  
  pinMode(timer_start, INPUT);
  pinMode(timer_stop, INPUT);
  pinMode(timer_rst, OUTPUT);

	
  
// **** SERIAL ПОРТ
  Serial.begin(9600);	
	
// проверить состояние данных в EEPROM  
// если в EEPROM есть корректные данные
// - восстановить значения уставок из EEPROM,
// - восстановить состояния из EEPROM
// - инициировать подключение к WiFi
	
// если данные в EEPROM не корректные, 	
	
	
	

// **** WiFi
  setup_wifi();
  
// **** MQTT 
  client.setServer(mqtt_server, mqtt_port);
  reconnect();
  
// **** BME280  
  while (!bme.begin() && int ct < 30) {            		        // до тех пор, пока BME280 не обнаружен   
    Serial.println ("bme280 not found");        			// сообщение "bme280 not found"               
    digitalWrite(LED_BUILTIN, LOW);            			
    delay(500);								// мигать 30 s
    digitalWrite(LED_BUILTIN, HIGH);             			// встроенным светодиодом
    delay(500);								// с частотой 1 Hz
    ct += 1:
  }
	
  
	
	
	

  delay(500);  
  PreTime = millis();               		        		// отметка времени в конце цикла программы 
}

// ------------------------------------------------------------
//                 LOOP - Основной цикл
// ------------------------------------------------------------

void loop() {                   

  if (!client.connected()) {				            	// если нет подключения к MQTT брокеру, 
    reconnect();							// переподключение и подписка на топики
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


  
  
}



