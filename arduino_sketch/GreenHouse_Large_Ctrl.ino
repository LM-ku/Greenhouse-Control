/* ------------------------------------------------------------
                      GREENHOUSE CTRL
   ------------------------------------------------------------

BME280 I2C (����������� �������, ��������, ��������� �������)
     Vin (Voltage In)     ->  3.3V
     Gnd (Ground)         ->  Gnd
     SDA (Serial Data)    ->  JPIO4 (D2) on D1 Mini
     SCK (Serial Clock)   ->  JPIO5 (D1) on D1 Mini
	 
CAPACITIVE SOIL MOISTURE SENSOR (��������� �����)	 
	 Vin (Voltage In)     ->  3.3V
     Gnd (Ground)         ->  Gnd
     AI  (Analog Input)   ->  ADC0 (A0) on D1 Mini 

PULS FLOW SENSOR (������ ���� �� �����)
	 Vin (Voltage In)     ->  3.3V
     Gnd (Ground)         ->  Gnd
     DI  (Digital Input)  ->  JPIO13 (D7) on D1 Mini	 

OUT VALVE RELAY (��������)
     DO  (Digital Output) ->  JPIO16 (D0) on D1 Mini	 

FAN RELAY (����������)
     DO  (Digital Output) ->  JPIO14 (D5) on D1 Mini	

IN VALVE RELAY (���������� ���� � ����� - ������)
     DO  (Digital Output) ->  JPIO12 (D6) on D1 Mini
	 
	 
*/

// **** ������������ ���������� 
#include <ESP8266WiFi.h>					// ���������� ESP8266WiFi
#include <PubSubClient.h>					// ���������� PubSubClient (MQTT)
#include <ESP8266WebServer.h>				// ���������� ESP8266WebServer
#include <BME280I2C.h>              		// ���������� ���280I2C
// #include <OLED_I2C.h>             		// ���������� OLED_I2C

// **** ��������� WiFi ����������� 
const char* ssid     = "******";            // �������� ����� �������     
const char* password = "******";            // ������                     

// **** ��������� MQTT ����������� 
const char* mqtt_server = "192.168.123.222";
WiFiClient espClient;
PubSubClient client(espClient);
long lastMsg = 0;
char msg[50];
int value = 0;

     
// **** ���������� � ������ BME280
// bme(<Temperature Oversampling Rate>, <Humidity Oversampling Rate>, <Pressure Oversampling Rate>, <Mode>, <Standby Time>, <Filter>, <SPI Enable>, <BME280 Address>)
// BME280I2C bme(0x1, 0x1, 0x1, 0x1, 0x5, 0x0, false, 0x77); // Version for SparkFun BME280

BME280I2C bme;								// �� ���������

  
// **** ���������� ����������

unsigned long PreTime;                                                              // ������� ���������� ������� � ���������� �����

long Cycle;                                                                         // ��, ������������ �����

float temp, hum, pres, moist
//float temp(NAN), hum(NAN), pres(NAN)

int year, month, day_m, day_w, hour, minute, second, ms								// ���������� ��� ����� �������
  
struct {
  float temp = 0;
  float hum = 0;
  float pres = 0;
  int counter = 0;
} avrg;                     // 3 float ���������� ��� ������� ������� �������� � �������

byte wait_cnt = 0;                // ������� ???





// ------------------------------------------------------------
//          SETUP_WiFi - ��������� ����������� � WiFi
// ------------------------------------------------------------
void setup_wifi() {
  delay(10);
  Serial.print("Connecting to ");					// ��������� "Connecting to "
  Serial.println(ssid);								// ��������� "<��� WiFi ����>"
  WiFi.mode(WIFI_STA);								// ����� ����������� � WiFi
  WiFi.begin(ssid, password);						// � ����� ������������� <ssid> � <password>
  while (WiFi.status() != WL_CONNECTED) {			// �� ��� ���, ���� ��� WiFi �����������
    delay(500);
    Serial.print(".");								// ��������� "..... "
  }
  Serial.println("");								// ����� ������������ ���������� � WiFi
  Serial.println("WiFi connected");					// ��������� "WiFi connected" 
  Serial.println("IP address: ");					// ��������� "IP address: "
  Serial.println(WiFi.localIP());					// ��������� "<WiFi.localIP>"
}


// ------------------------------------------------------------
//           RECONNECT - ��������� ����������� � MQTT
// ------------------------------------------------------------
void reconnect() {
  while (!client.connected()) {						// �� ��� ���, ���� ��� ����������� � MQTT
    Serial.print("Attempting MQTT connection...");	// ��������� "Attempting MQTT connection..."
    String clientId = "GreenHouse_Large_Ctrl";		// ID MQTT-�������
    if (client.connect(clientId))					// � ����� ������ : client.connect(clientId,userName,passWord) 
    {												// ���� ������ ����������� � �������, 
      Serial.println("connected");					// ��������� "connected"
      client.subscribe("GreenHouse_Large/�md");		// �������� �� �������
    } else {										// �����,
      Serial.print("failed, rc=");					// ��������� "failed, rc="
      Serial.print(client.state());					// ��������� "<��� ���������>"
      Serial.println(" try again in 10 seconds");	// ��������� " try again in 10 seconds"
      delay(10000);									// ����� ����� ��������� ����������� 10 s
    }
  }
}











// ------------------------------------------------------------
//                 SETUP - �������������
// ------------------------------------------------------------

void setup() {                  
  PreTime = millis();                                                               // ��������� ��������� ����� 

  // **** ������������� �������
  pinMode(LED_BUILTIN, OUTPUT);				// ���������� ���������
  pinMode(LED_BUILTIN, OUTPUT);				// ���������� ���������
  digitalWrite(LED_BUILTIN, HIGH);			// ������
  delay(100);								// �� 100 ms
  digitalWrite(LED_BUILTIN, LOW);			// ��������  
  

  pinMode(13, INPUT);						// ����������� �����������

  pinMode(12, OUTPUT);						// ����������� ���� ������� ���������� ����
  pinMode(14, OUTPUT);						// ����������� ���� ����������
  pinMode(16, OUTPUT);  					// ����������� ���� ������� ������
 
  
// **** ���� ��� ��������������� ���������
  Serial.begin(9600);						 

// **** ����������� � WiFi
  setup_wifi();
  
// **** ���������� MQTT �������
  client.setServer(mqtt_server, 1883);

  
// **** ����������� � BME280  
  while (!bme.begin()) {            		// �� ��� ���, ���� BME280 �� ���������              
    delay(500);								// ������
    digitalWrite(LED_BUILTIN, HIGH);		// ���������� �����������
    delay(500);								// � �������� 1 Hz
    digitalWrite(LED_BUILTIN, LOW);
    Serial.println ("bme280 not found");	// ��������� "bme280 not found"
  }


  
  
  delay(500);  
  Cycle = millis() - PreTime;                                                        // ������ ������� � ����������� ����� ��������� 
}

// ------------------------------------------------------------
//                 LOOP - �������� ����
// ------------------------------------------------------------

void loop() {                   

   if (!client.connected()) {				// ����������� � ������� � �������� �� ������
    reconnect();
  } 



  sensors_rd();								// ����� ��������




  
  delay(1000);


// **** ��������� �����  
  Cycle = millis() - PreTime;                                                        // ������ ������� � ����������� ����� ��������� 
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
//                 SENSORS_RD - ����� ��������
// ------------------------------------------------------------
void sensors_rd() {

// **** ����� ������� BME280
  float temp(NAN), hum(NAN), pres(NAN);
  bool metric = true;
  uint8_t pressureUnit(0);   						// B000 = Pa, B001 = hPa, B010 = Hg, B011 = atm, B100 = bar, B101 = torr, B110 = N/m^2, B111 = psi             	
  bme.read(pres, temp, hum, metric, pressureUnit);  // ������� �������� ��������, �����������, ���������, � ����������� �������, �������� � �� 
//  temp -= 0.3;                  					// correct temp
  pres /= 133.3;                  					// convert to mmHg
  
// **** ����� ������� ��������� �����  
  const int air_val = 520;							// ��������� ������� �� �������, ������������� 0%
  const int water_val = 260;  						// ��������� ������� � ����, ������������� 100%
  int sensor_moist = analogRead(0);
  
  if sensor_moist <= air_val {
	sensor_moist = air_val;
  }
  if sensor_moist >= water_val {
	sensor_moist = water_val;
  }
  moist = 100.0*(air_val-sensor_moist)/(air_val-water_val);
  
// *** ����� ���������� � ��������
  Serial.print ("����������� �������   ");
  Serial.print (temp);
  Serial.println ("   *C");
  
  Serial.print ("��������� �������   ");
  Serial.print (hum);
  Serial.println ("   %");
  
  Serial.print ("��������   ");
  Serial.print (pres);
  Serial.println ("   mmHg");

  Serial.print ("��������� �����   ");
  Serial.print (moist);
  Serial.print ("   %  ["); 
  Serial.print (sensor_moist);  
  Serial.println ("]"); 
}
