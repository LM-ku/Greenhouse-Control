/*
 
Синхронизация времени с помощью NTP-сервера и WiFi
 
*/
 
#include <TimeLib.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
 
const char ssid[] = "Mi Phone";  //  SSID (название) вашей сети
const char pass[] = "Yeti_t671ex";       // пароль к вашей сети
 
// NTP-серверы:

//static const char ntpServerName[] = "ntp1.stratum2.ru";
//static const char ntpServerName[] = "ntp2.stratum2.ru";
//static const char ntpServerName[] = "ntp3.stratum2.ru";
//static const char ntpServerName[] = "ntp4.stratum2.ru";
//static const char ntpServerName[] = "ntp5.stratum2.ru";
//static const char ntpServerName[] = "ntp1.stratum1.ru";
//static const char ntpServerName[] = "ntp2.stratum1.ru";
//static const char ntpServerName[] = "ntp3.stratum1.ru";
//static const char ntpServerName[] = "ntp4.stratum1.ru";
static const char ntpServerName[] = "ntp5.stratum1.ru";

//static const char ntpServerName[] = "us.pool.ntp.org";
//static const char ntpServerName[] = "time.nist.gov";
//static const char ntpServerName[] = "time-a.timefreq.bldrdoc.gov";
//static const char ntpServerName[] = "time-b.timefreq.bldrdoc.gov";
//static const char ntpServerName[] = "time-c.timefreq.bldrdoc.gov";
 
const int timeZone = 5;     
//const int timeZone = 1;     // центрально-европейское время
//const int timeZone = -5;  // восточное время (США)
//const int timeZone = -4;  // восточное дневное время (США)
//const int timeZone = -8;  // тихоокеанское время (США)
//const int timeZone = -7;  // тихоокеанское дневное время (США) 
 
 
WiFiUDP Udp;
unsigned int localPort = 2390;  // локальный порт для прослушивания UDP-пакетов
 
time_t getNtpTime();
time_t mem = 0;
boolean st = false;

void digitalClockDisplay();
void printDigits(int digits);
void sendNTPpacket(IPAddress &address);
 
void setup()
{
  pinMode(2, INPUT);           // назначить выводу порт ввода
  
  Serial.begin(9600);
  //while (!Serial) ; // нужно только для Leonardo
  delay(250);
  Serial.println("TimeNTP Example");  //  "Синхронизация с помощью NTP"
  Serial.print("Connecting to ");  //  "Подключаемся к "
  Serial.println(ssid);
  WiFi.begin(ssid, pass);
 
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  

  Serial.print("IP number assigned by DHCP is ");  //  "IP, присвоенный DHCP: "
  Serial.println(WiFi.localIP());
  Serial.println("Starting UDP");  //  "Начинаем UDP"
  Udp.begin(localPort);
  Serial.print("Local port: ");  //  "Локальный порт: "
  Serial.println(Udp.localPort());
  Serial.println("waiting for sync");  //  "ждем синхронизации"
  setSyncProvider(getNtpTime);
  setSyncInterval(300);

  mem = now();
}



void ttt()
{
  if (digitalRead(2) == LOW && !st) {
    st = true;
    // прошло времени c предыдущей синхронизации
    unsigned long interval = now() - mem;
    Serial.println("С момента предыдущего запроса прошло  :");
    Serial.print("дней = ");
    Serial.print(day(interval)-1);
    Serial.print("  часов = ");
    Serial.print(hour(interval));
    Serial.print("  минут = ");
    Serial.print(minute(interval));
    Serial.print(" / ");
    Serial.print(interval);
    Serial.print(" / ");
    Serial.println(st);
    // сохранить  метку времени
    //mem = now();
  }
}

time_t prevDisplay = 0; // когда будут показаны цифровые часы
 
 
void loop()
{
  if (timeStatus() != timeNotSet) {
    if (now() != prevDisplay) {  // обновляем дисплей только если время поменялось
      prevDisplay = now();
      digitalClockDisplay();      
    }
  }
  ttt();
  if (digitalRead(2) == HIGH)  st = false;
}
 
void digitalClockDisplay()
{
  // показываем цифровые часы: 
  Serial.print(hour());
  printDigits(minute());
  printDigits(second());
  Serial.print(" ");
  Serial.print(day());
  Serial.print(".");
  Serial.print(month());
  Serial.print(".");
  Serial.print(year());
  Serial.print("  now() = ");
  Serial.print(now());
  //Serial.print("  gpio 2 = ");
  //Serial.print(digitalRead(2));
  Serial.println();
}
 
void printDigits(int digits)
{
  // вспомогательная функция для печати данных о времени 
  // на монитор порта; добавляет в начале двоеточие и ноль:
  Serial.print(":");
  if (digits < 10)
    Serial.print('0');
  Serial.print(digits);
}
 
/*-------- Код для NTP ----------*/
 
const int NTP_PACKET_SIZE = 48;  //  NTP-время – в первых 48 байтах сообщения
byte packetBuffer[NTP_PACKET_SIZE];  //  буфер для хранения входящих и исходящих пакетов 
 
time_t getNtpTime()
{
  IPAddress ntpServerIP; // IP-адрес NTP-сервера
 
  while (Udp.parsePacket() > 0) ; // отбраковываем все пакеты, полученные ранее 
  Serial.println("Transmit NTP Request");  //  "Передача NTP-запроса" 
  // подключаемся к случайному серверу из списка:
  WiFi.hostByName(ntpServerName, ntpServerIP);
  Serial.print(ntpServerName);
  Serial.print(": ");
  Serial.println(ntpServerIP);
  sendNTPpacket(ntpServerIP);
  uint32_t beginWait = millis();
  while (millis() - beginWait < 1500) {
    int size = Udp.parsePacket();
    if (size >= NTP_PACKET_SIZE) {
      Serial.println("Receive NTP Response");  //  "Получение NTP-ответа"
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
 
// отправляем NTP-запрос серверу времени по указанному адресу: 
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
