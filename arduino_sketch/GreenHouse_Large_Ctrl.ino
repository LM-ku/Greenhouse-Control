/* ------------------------------------------------------------
                      GREENHOUSE CTRL
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
#include <ESP8266WiFi.h>					// библиотека ESP8266WiFi
#include <PubSubClient.h>					// библиотека PubSubClient (MQTT)
#include <ESP8266WebServer.h>				// библиотека ESP8266WebServer
#include <BME280I2C.h>              		// библиотека ВМЕ280I2C
// #include <OLED_I2C.h>             		// библиотека OLED_I2C

// **** НАСТРОЙКИ WiFi ПОДКЛЮЧЕНИЯ 
const char* ssid     = "******";            // Название точки доступа     
const char* password = "******";            // Пароль                     

// **** НАСТРОЙКИ MQTT ПОДКЛЮЧЕНИЯ 
const char* mqtt_server = "192.168.123.222";
WiFiClient espClient;
PubSubClient client(espClient);
long lastMsg = 0;
char msg[50];
int value = 0;

     
// **** НАЗНАЧЕНИЕ И РЕЖИМЫ BME280
// bme(<Temperature Oversampling Rate>, <Humidity Oversampling Rate>, <Pressure Oversampling Rate>, <Mode>, <Standby Time>, <Filter>, <SPI Enable>, <BME280 Address>)
// BME280I2C bme(0x1, 0x1, 0x1, 0x1, 0x5, 0x0, false, 0x77); // Version for SparkFun BME280

BME280I2C bme;								// По умолчанию

  
// **** ОБЪЯВЛЕНИЕ ПЕРЕМЕННЫХ

unsigned long PreTime;                                                              // отметка системного времени в предыдущем цикле

long Cycle;                                                                         // мс, длительность цикла

float temp, hum, pres, moist
//float temp(NAN), hum(NAN), pres(NAN)

int year, month, day_m, day_w, hour, minute, second, ms								// переменные для учета времени
  
struct {
  float temp = 0;
  float hum = 0;
  float pres = 0;
  int counter = 0;
} avrg;                     // 3 float переменных для расчета средних значений и счетчик

byte wait_cnt = 0;                // счетчик ???





// ------------------------------------------------------------
//          SETUP_WiFi - ПРОЦЕДУРА ПОДКЛЮЧЕНИЯ К WiFi
// ------------------------------------------------------------
void setup_wifi() {
  delay(10);
  Serial.print("Connecting to ");					// сообщение "Connecting to "
  Serial.println(ssid);								// сообщение "<имя WiFi сети>"
  WiFi.mode(WIFI_STA);								// старт подключения к WiFi
  WiFi.begin(ssid, password);						// с ранее определенными <ssid> и <password>
  while (WiFi.status() != WL_CONNECTED) {			// до тех пор, пока нет WiFi подключения
    delay(500);
    Serial.print(".");								// сообщение "..... "
  }
  Serial.println("");								// после установления соединения с WiFi
  Serial.println("WiFi connected");					// сообщение "WiFi connected" 
  Serial.println("IP address: ");					// сообщение "IP address: "
  Serial.println(WiFi.localIP());					// сообщение "<WiFi.localIP>"
}


// ------------------------------------------------------------
//           RECONNECT - ПРОЦЕДУРА ПОДКЛЮЧЕНИЯ К MQTT
// ------------------------------------------------------------
void reconnect() {
  while (!client.connected()) {						// до тех пор, пока нет подключения к MQTT
    Serial.print("Attempting MQTT connection...");	// сообщение "Attempting MQTT connection..."
    String clientId = "GreenHouse_Large_Ctrl";		// ID MQTT-клиента
    if (client.connect(clientId))					// в общем случае : client.connect(clientId,userName,passWord) 
    {												// если клиент подключился к брокеру, 
      Serial.println("connected");					// сообщение "connected"
      client.subscribe("GreenHouse_Large/Сmd");		// подписка на команды
    } else {										// иначе,
      Serial.print("failed, rc=");					// сообщение "failed, rc="
      Serial.print(client.state());					// сообщение "<код состояния>"
      Serial.println(" try again in 10 seconds");	// сообщение " try again in 10 seconds"
      delay(10000);									// пауза перед повторным соединением 10 s
    }
  }
}











// ------------------------------------------------------------
//                 SETUP - Инициализация
// ------------------------------------------------------------

void setup() {                  
  PreTime = millis();                                                               // сохранить системное время 

  // **** ИНИЦИАЛИЗАЦИЯ ВЫВОДОВ
  pinMode(LED_BUILTIN, OUTPUT);				// встроенный светодиод
  pinMode(LED_BUILTIN, OUTPUT);				// встроенный светодиод
  digitalWrite(LED_BUILTIN, HIGH);			// зажечь
  delay(100);								// на 100 ms
  digitalWrite(LED_BUILTIN, LOW);			// погасить  
  

  pinMode(13, INPUT);						// подключение расходомера

  pinMode(12, OUTPUT);						// подключение реле клапана наполнения бака
  pinMode(14, OUTPUT);						// подключение реле вентиляции
  pinMode(16, OUTPUT);  					// подключение реле клапана полива
 
  
// **** ПОРТ ДЛЯ ДИАГНОСТИЧЕСКИХ СООБЩЕНИЙ
  Serial.begin(9600);						 

// **** ПОДКЛЮЧЕНИЕ К WiFi
  setup_wifi();
  
// **** НАЗНАЧЕНИЕ MQTT БРОКЕРА
  client.setServer(mqtt_server, 1883);

  
// **** ПОДКЛЮЧЕНИЕ К BME280  
  while (!bme.begin()) {            		// до тех пор, пока BME280 не обнаружен              
    delay(500);								// мигать
    digitalWrite(LED_BUILTIN, HIGH);		// встроенным светодиодом
    delay(500);								// с частотой 1 Hz
    digitalWrite(LED_BUILTIN, LOW);
    Serial.println ("bme280 not found");	// сообщение "bme280 not found"
  }


  
  
  delay(500);  
  Cycle = millis() - PreTime;                                                        // прошло времени с предыдущего цикла программы 
}

// ------------------------------------------------------------
//                 LOOP - Основной цикл
// ------------------------------------------------------------

void loop() {                   

   if (!client.connected()) {				// подключение к брокеру и подписка на топики
    reconnect();
  } 



  sensors_rd();								// опрос датчиков




  
  delay(1000);


// **** СИСТЕМНОЕ ВРЕМЯ  
  Cycle = millis() - PreTime;                                                        // прошло времени с предыдущего цикла программы 
  ms = ms + Cycle;
  if ms >= 1000 {
	ms -= 1000;
	second += 1;
  }
  if second >= 60 {
    second -= 60;
    minute += 1;
  }
  if minute >=60 {
    minute -=60;
	hour += 1;
  }
  if hour >= 24 {
    hour -= 24;
	day_m += 1;
    day_w += 1;
  }
  if day_w >= 8 {
    day_w = 1;
  }
  if (day_m >= 28) && (month == 2) && (year % 4 != 0) {
    day_m -= 28;
	month = 3;
  }
  if (day_m >= 29) && (month == 2) && (year % 4 == 0) {
    day_m -= 29;
	month = 3;
  }
  if (day_m >= 30) && ((month == 4) || (month == 6) || (month == 9) || (month == 11)) {
    day_m -= 30;
	month += 1;
  }
  if (day_m >= 31) && ((month == 1) || (month == 3) || (month == 5) || (month == 7) || (month == 8) || (month == 10)) {
    day_m -= 31;
	month += 1;
  }
  if (day_m >= 31) && (month == 12)  {
    day_m -= 31;
	month = 1;
	year += 1;
  }
  PreTime = millis(); 
  
  
}


// ------------------------------------------------------------
//                 SENSORS_RD - Опрос датчиков
// ------------------------------------------------------------
void sensors_rd() {

// **** ОПРОС ДАТЧИКА BME280
  float temp(NAN), hum(NAN), pres(NAN);
  bool metric = true;
  uint8_t pressureUnit(0);   						// B000 = Pa, B001 = hPa, B010 = Hg, B011 = atm, B100 = bar, B101 = torr, B110 = N/m^2, B111 = psi             	
  bme.read(pres, temp, hum, metric, pressureUnit);  // считать значения давления, температуры, влажности, в метрической системе, давление в Ра 
//  temp -= 0.3;                  					// correct temp
  pres /= 133.3;                  					// convert to mmHg
  
// **** ОПРОС ДАТЧИКА ВЛАЖНОСТИ ПОЧВЫ  
  const int air_val = 520;							// показания датчика на воздухе, соответствуют 0%
  const int water_val = 260;  						// показания датчика в воде, соответствуют 100%
  int sensor_moist = analogRead(0);
  
  if sensor_moist <= air_val {
	sensor_moist = air_val;
  }
  if sensor_moist >= water_val {
	sensor_moist = water_val;
  }
  moist = 100.0*(air_val-sensor_moist)/(air_val-water_val);
  
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
