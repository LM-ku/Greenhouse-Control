
/*
 Vin (Voltage In)    ->  3.3V
 Gnd (Ground)        ->  Gnd
 SDA (Serial Data)   ->  JPIO4 on D1 Mini
 SCK (Serial Clock)  ->  JPIO5 on D1 Mini
*/

#include <BME280I2C.h>              // библиотека ВМЕ280I2C
// #include <OLED_I2C.h>             // библиотека OLED_I2C

#define PLOT_LEN      100           
#define STORAGE_TIME  270

// OLED  myOLED(SDA, SCL, 8);
BME280I2C bme;

// Temperature Oversampling Rate, Humidity Oversampling Rate, Pressure Oversampling Rate, Mode, Standby Time, Filter, SPI Enable, BME280 Address
// BME280I2C bme(0x1, 0x1, 0x1, 0x1, 0x5, 0x0, false, 0x77); // Version for SparkFun BME280
  
extern uint8_t BigNumbers[];
extern uint8_t SmallFont[];

struct {
  byte temp = 0;
  byte hum = 0;
  byte pres = 0;
} infoArr[PLOT_LEN];              // 3 byte массива по PLOT_LEN = 100 значений для графиков

struct {
  float temp = 0;
  float hum = 0;
  float pres = 0;
  int counter = 0;
} avrg;                     // 3 float переменных для расчета средних значений и счетчик

byte wait_cnt = 0;                // счетчик ???
bool fastMode = true;

void setup() {                  // предустановки
  Serial.begin(9600);
  //myOLED.begin();
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(100);
  digitalWrite(LED_BUILTIN, LOW);

  while (!bme.begin()) {            // if bme280 not found - blink LED              
    delay(500);
    digitalWrite(LED_BUILTIN, HIGH);
    delay(500);
    digitalWrite(LED_BUILTIN, LOW);
    Serial.println ("bme280 not found");
  }
  delay(500);
}

void loop() {                   // основной цикл
  bool metric = true;
  float temp(NAN), hum(NAN), pres(NAN);
  uint8_t pressureUnit(0);              // unit: B000 = Pa, B001 = hPa, B010 = Hg, B011 = atm, B100 = bar, B101 = torr, B110 = N/m^2, B111 = psi

  bme.read(pres, temp, hum, metric, pressureUnit);  // считать значения давления, температуры, влажности, в метрической системе, давление в Ра 
//  temp -= 0.3;                  // correct temp
  pres /= 133.3;                  // convert to mmHg

//  myOLED.setBrightness(10);             // яркость дисплея = 10
//  myOLED.clrScr();                  // очистить экран дисплея
//  myOLED.setFont(BigNumbers);           // установить большой шрифт
//  myOLED.print(String(temp, 1), 0, 0);        // вывести на экран значение температуры в координатах 0,0
//  myOLED.print(String(hum, 0), 92, 0);        // вывести на экран значение влажности в координатах 92,0
//  myOLED.print(String(pres, 1), 42, 40);      // вывести на экран значение давления в координатах 42,40

//  myOLED.setFont(SmallFont);            // установить маленький шрифт
//  myOLED.print("~C", 56, 0);            // вывести на экран "*С" в координатах 56,0
//  myOLED.print("%", 122, 0);            // вывести на экран "%" в координатах 122,0
//  myOLED.print("MM", 114, 58);            // вывести на экран "ММ" в координатах 114,58
//  myOLED.();                  // обновить экран дисплея

  Serial.print ("Температура   ");
  Serial.print (temp);
  Serial.println ("   *C");
  
  Serial.print ("Влажность   ");
  Serial.print (hum);
  Serial.println ("   %");
  
  Serial.print ("Давление   ");
  Serial.print (pres);
  Serial.println ("   mmHg");

//  avrg.temp += temp;                // сумма температуры для расчета среднего значения
//  avrg.hum += hum;                  // сумма влажности для расчета среднего значения
//  avrg.pres += pres;                // сумма давлений для расчета среднего значения
//  avrg.counter++;                 // счетчик для расчета среднего значения

//  if (fastMode && avrg.counter >= STORAGE_TIME) { // после получения >= STORAGE_TIME значений в режиме fastMode
//    fastMode = false;               // отключить fastMode
//    for (int i = 0; i < PLOT_LEN - 1; i++) {    // обнулить массивы данных для графиков
//      infoArr[i].temp = 0;              // ... температура
//      infoArr[i].hum = 0;             // ... влажность
//      infoArr[i].pres = 0;              // ... давление
//    }
//  }

//  if (fastMode || avrg.counter >= STORAGE_TIME) { // в режиме fastMode или после накопления очередных STORAGE_TIME измерений
//    if (avrg.counter >= STORAGE_TIME) {       // расчитать средние значения по STORAGE_TIME измерениям
//      temp = avrg.temp / avrg.counter;        // ... температура
//      hum = avrg.hum / avrg.counter;        // ... влажность
//      pres = avrg.pres / avrg.counter;        // ... давление
//      avrg.temp = 0;                // инициализация переменных для нового расчета средних значений
//      avrg.hum = 0;
//      avrg.pres = 0;
//      avrg.counter = 0;
//    }
//    for (int i = 1; i < PLOT_LEN; i++) {      // сдвиг данных в массивах графиков
//      infoArr[i - 1] = infoArr[i];
//    }
//    infoArr[PLOT_LEN - 1].temp = round(temp) + 50;  // формирование данных для графика температуры (сдвиг вверх на 50)
//    infoArr[PLOT_LEN - 1].pres = round(pres) - 650; // формирование данных для графика давления (отсечь 650 снизу)
//    infoArr[PLOT_LEN - 1].hum = round(hum);     // формирование данных для графика влажности (как есть)
//  }
  delay(1000);

  /*
    Graph
  */

/*  if (wait_cnt > 3) {               // раз в 3 секунды ...
    wait_cnt = 0;
    myOLED.clrScr();                // очистить экран дисплея
                          // инициализация значений
    byte minTemp = 255;               // MIN температуры
    byte minHum = 255;                // MIN влажности
    byte minPres = 255;               // MIN давления
    byte maxTemp = 0;               // MAX температуры
    byte maxHum = 0;                // MAX влажности
    byte maxPres = 0;               // MAX давления

    for (int i = PLOT_LEN - 1; i >= 0 ; i--) {    // для всех не нулевых измерений ...
      if (infoArr[i].temp == 0 && infoArr[i].hum == 0 && infoArr[i].pres == 0) break;

      if (infoArr[i].temp < minTemp) minTemp = infoArr[i].temp; // нахождение минимальных значений
      if (infoArr[i].hum < minHum) minHum = infoArr[i].hum;
      if (infoArr[i].pres < minPres) minPres = infoArr[i].pres;

      if (infoArr[i].temp > maxTemp) maxTemp = infoArr[i].temp; // нахождение максимальных значений
      if (infoArr[i].hum > maxHum) maxHum = infoArr[i].hum;
      if (infoArr[i].pres > maxPres) maxPres = infoArr[i].pres;
    }
    if (maxTemp - minTemp < 10) maxTemp = minTemp + 10; 
    if (maxHum - minHum < 10) maxHum = minHum + 10;
    if (maxPres - minPres < 10) maxPres = minPres + 10;


    myOLED.setFont(SmallFont);            // установить маленький шрифт
    myOLED.print(String(minTemp - 50), 0, 12);    // вывести на экран дисплея MIN значение температуры
    myOLED.print(String(maxTemp - 50), 0, 2);   // вывести на экран дисплея MAX значение температуры

    myOLED.print(String(minHum), 0, 34);      // вывести на экран дисплея MIN значение влажности
    myOLED.print(String(maxHum), 0, 24);      // вывести на экран дисплея MAX значение влажности

    myOLED.print(String(minPres + 650), 0, 56);   // вывести на экран дисплея MIN значение давления
    myOLED.print(String(maxPres + 650), 0, 46);   // вывести на экран дисплея MAX значение давления

    int x = 24;
    for (int i = 0; i < PLOT_LEN - 1; i++) {    // вывести на экран дисплея с поз.x=24 графики температуры, влажности и давления
      if (infoArr[i].temp == 0 && infoArr[i].hum == 0 && infoArr[i].pres == 0) continue;

      myOLED.drawLine(x, map(infoArr[i].temp, minTemp, maxTemp, 18, 0), x + 1, map(infoArr[i + 1].temp, minTemp, maxTemp, 18, 0));
      myOLED.drawLine(x, map(infoArr[i].hum, minHum, maxHum, 40, 22), x + 1, map(infoArr[i + 1].hum, minHum, maxHum, 40, 22));
      myOLED.drawLine(x, map(infoArr[i].pres, minPres, maxPres, 62, 44), x + 1, map(infoArr[i + 1].pres, minPres, maxPres, 62, 44));

      x++;
    }

    myOLED.update();                // обновить экран дисплея

    delay(2000);
  }

  wait_cnt++;
  */
}
