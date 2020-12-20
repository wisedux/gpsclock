/*
 * GPS Clock Syncer
 * 
 * Part 1 of 2 for a GPS driven IRIG-B wall clock.
 * 
 * - Gets time sync from GPS receiver
 * - I2C slave that sends next second time data to IRIG-B generator
 * - Outputs current time to LCD panel
 */

#include <LiquidCrystal.h>
//#include <TimeLib.h>
#include <TinyGPS.h>       // http://arduiniana.org/libraries/TinyGPS/
#include <SoftwareSerial.h>
#include <Wire.h>
//#include <RTClib.h>     // https://github.com/mizraith/RTClib
//#include <RTC_DS3231.h> // https://github.com/mizraith/RTClib

TinyGPS gps;

/*
   Set GPS serial pins
   Can't use SoftwareSerial if we need to be picky with timer interrupts
   (interrupts are disabled during TX/RX)
*/
// redboard: GPS TX pin 8; GPS RX pin 9
SoftwareSerial SerialGPS(8, 9);
// trinket: GPS TX pin 4; GPS RX pin 5
//SoftwareSerial SerialGPS(4, 5);
// GPS TX pin 0; GPS RX pin 1
//#define SerialGPS Serial

// LCD pin assignments
// redboard
const byte rs = 12, en = 11, d4 = 7, d5 = 6, d6 = 5, d7 = 4;
// pro trinket
//const byte rs = 8, en = 6, d4 = 10, d5 = 11, d6 = 12, d7 = 13;
//const byte rs = 7, en = 8, d4 = 9, d5 = 10, d6 = 11, d7 = 12;
//const byte rs = 12, en = 11, d4 = 5, d5 = 4, d6 = 3, d7 = 2;
LiquidCrystal lcd(rs, en, d4, d5, d6, d7);

// 1PPS (1 pulse per second) pin on GPS module
// has 10k pulldown resistor
// redboard
const byte PPS_pin = 2;
// pro trinket
//const byte PPS_pin = 3;
//const byte PPS_pin = 7;


#define I2C_SLAVE_ADDR 9

volatile byte updateLCD = 0;  // Bitmask telling main loop to update the LCD
// OR these bits together to update various strings on LCD
#define LCD_UPDATE_DATE B00000001
#define LCD_UPDATE_TIME B00000010

typedef struct irig_datetime {
  byte second, minute, hour;
  byte day, month;   // not used by IRIG-B
  unsigned int year;
  unsigned int dayOfYear;
  unsigned long sbs; // straight binary seconds (17-bit number)
};

// Two rotating datetime structs; one is set after NMEA parsed
irig_datetime dt1;
irig_datetime dt2;

// Pointers for next (written) and current (read) datetime
volatile irig_datetime* nextDt;
volatile irig_datetime* currentDt;
// A zeroed out datetime is sent over I2C if a valid time can't be sent
const irig_datetime dt0 = {};
const irig_datetime* zeroDt = &dt0;

//volatile bool clockValid = false; // Prevent displaying 2000-00-00 00:00:01 at startup
// FIXME
volatile bool nextDtValid = false;
volatile bool currentDtValid = false;

volatile unsigned int ticks = 0;  // Number of timer1 interrupts since 1PPS pulse
bool blinkState = true; // Toggles each second: used for blinking stuff on LCD
char colon = ':';

void setup()
{  
  Wire.begin(I2C_SLAVE_ADDR);
  Wire.onRequest(requestEvent);
  //Wire.onReceive(receiveEvent);

  SerialGPS.begin(9600);
  delay(500);
  lcd.begin(20, 4);

  // Look for pulse on 1PPS pin
  attachInterrupt(digitalPinToInterrupt(PPS_pin), ppsInterrupt, RISING);
}

// Send next second's datetime when requested, if valid
void requestEvent() {
//  Wire.write((char *) nextDt, sizeof(irig_datetime));
  if (nextDtValid)
    Wire.write((char *) nextDt, sizeof(irig_datetime));
  else
    Wire.write((char *) zeroDt, sizeof(irig_datetime));
}

// ISR to
// * send IRIG start-of-second marker when 1PPS pin goes high
// * reset variables to start of second conditions
void ppsInterrupt () {
  //digitalWriteFast(IRIG_pin, HIGH);
  //ticks = 0;
  // If NMEA sentence couldn't be parsed last second it will start out true
  //parseNMEA = false;
  // Swap current/next datetime pointers
  if (currentDt == &dt1) {
    currentDt = &dt2;
    nextDt = &dt1;
  } else {
    currentDt = &dt1;
    nextDt = &dt2;
  }
  
  currentDtValid = nextDtValid;
  nextDtValid = false;
  updateLCD = LCD_UPDATE_DATE | LCD_UPDATE_TIME;
}

void updateNextDt() {
  // when TinyGPS reports new data...
  unsigned long age;
  word Year;
  byte Month, Day, Hour, Minute, Second;
  gps.crack_datetime(&Year, &Month, &Day, &Hour, &Minute, &Second, NULL, &age);
  nextDtValid = false;
  if (age < 500) {
    // Add 1 second to time since it's already out of date
    nextDt->second = Second + 1;
    if (nextDt->second <= 59) {
      nextDt->minute = Minute;
      nextDt->hour = Hour;
      nextDt->day = Day;
      nextDt->month = Month;
      nextDt->year = Year;
    } else {
      nextDt->second = 0;
      nextDt->minute = Minute + 1;
      if (nextDt->minute <= 59) {
        nextDt->hour = Hour;
        nextDt->day = Day;
        nextDt->month = Month;
        nextDt->year = Year;
      } else {
        nextDt->minute = 0;
        nextDt->hour = Hour + 1;
        if (Hour <= 23) {
          nextDt->day = Day;
          nextDt->month = Month;
          nextDt->year = Year;
        } else {
          nextDt->hour = 0;
          nextDt->day = Day + 1;
          // Forget about month and year rollover for now.
          nextDt->month = Month;
          nextDt->year = Year;
        }
      }
    }
    // Calculate straight binary seconds
    nextDt->sbs = (unsigned long)nextDt->hour * 3600 + (unsigned int)nextDt->minute * 60 + nextDt->second;
    //nextDt->sbs = (unsigned long) 0;

    // Calculate day of year
    // If March or later, we need to know whether it's a leap year
    byte leapYear;
    if (nextDt->month > 2) {
      //unsigned int fullYear = century * 100 + nextDt->year;
      if (nextDt->year % 4 != 0)
        leapYear = 0;
      else if (nextDt->year % 100 != 0)
        leapYear = 1;
      else if (nextDt->year % 400 != 0)
        leapYear = 0;
      else
        leapYear = 1;
    }
    switch (nextDt->month) {
      case 1: // January
        nextDt->dayOfYear = nextDt->day;
        break;
      case 2: // February
        nextDt->dayOfYear = 31 + nextDt->day;
        break;
      case 3: // March
        nextDt->dayOfYear = 31 + 28 + leapYear + nextDt->day;
        break;
      case 4: // April
        nextDt->dayOfYear = 31 + 28 + leapYear + 31 + nextDt->day;
        break;
      case 5: // May
        nextDt->dayOfYear = 31 + 28 + leapYear + 31 + 30 + nextDt->day;
        break;
      case 6: // June
        nextDt->dayOfYear = 31 + 28 + leapYear + 31 + 30 + 31 + nextDt->day;
        break;
      case 7: // July
        nextDt->dayOfYear = 31 + 28 + leapYear + 31 + 30 + 31 + 30 + nextDt->day;
        break;
      case 8: // August
        nextDt->dayOfYear = 31 + 28 + leapYear + 31 + 30 + 31 + 30 +
                    31 + nextDt->day;
        break;
      case 9: // September
        nextDt->dayOfYear = 31 + 28 + leapYear + 31 + 30 + 31 + 30 +
                    31 + 31 + nextDt->day;
        break;
      case 10: // October
        nextDt->dayOfYear = 31 + 28 + leapYear + 31 + 30 + 31 + 30 +
                    31 + 31 + 30 + nextDt->day;
        break;
      case 11: // November
        nextDt->dayOfYear = 31 + 28 + leapYear + 31 + 30 + 31 + 30 +
                    31 + 31 + 30 + 31 + nextDt->day;
        break;
      case 12: // December
        nextDt->dayOfYear = 31 + 28 + leapYear + 31 + 30 + 31 + 30 +
                    31 + 31 + 30 + 31 + 30 + nextDt->day;
        break;
    }
    nextDtValid = true;
  }
}

void loop()
{
  // Parse NMEA string when it's available
  while (SerialGPS.available()) {
    if (gps.encode(SerialGPS.read()))
      updateNextDt();
  }
  
  if (updateLCD) {
    if (doLCDupdate());
      updateLCD = 0;
  }
}

static bool doLCDupdate() {
  unsigned int bytesWritten = 0;

  if (updateLCD & LCD_UPDATE_TIME) {
    char time_disp[11];
    if (currentDtValid)
        sprintf(time_disp, " %02d%c%02d%c%02d+",
                        currentDt->hour, colon,
                        currentDt->minute, colon,
                        currentDt->second);
    else
        sprintf(time_disp, " %02d%c%02d%c%02d ",
                        currentDt->hour, colon,
                        currentDt->minute, colon,
                        currentDt->second);
    lcd.setCursor(10, 0);
    bytesWritten += lcd.print(time_disp);
    if (nextDtValid)
        sprintf(time_disp, " %02d%c%02d%c%02d+",
                        nextDt->hour, colon,
                        nextDt->minute, colon,
                        nextDt->second);
    else
        sprintf(time_disp, " %02d%c%02d%c%02d ",
                        nextDt->hour, colon,
                        nextDt->minute, colon,
                        nextDt->second);
    lcd.setCursor(10, 1);
    bytesWritten += lcd.print(time_disp);

    // Straight Binary Seconds
    if (currentDtValid) {
      char sbs_disp[6];
      sprintf(sbs_disp, "%05lu", currentDt->sbs);
      lcd.setCursor(14, 2);
      bytesWritten += lcd.print(sbs_disp);
    }
    
    // Show whether dt1 or dt2 was used
    lcd.setCursor(16, 3);
    if (currentDt == &dt1)
      bytesWritten += lcd.print("dt1");
    else
      bytesWritten += lcd.print("dt2");
    
  }

  if (updateLCD & LCD_UPDATE_DATE) {
    char date_disp[11];
    sprintf(date_disp, "%04d-%02d-%02d", currentDt->year, currentDt->month, currentDt->day);
    lcd.setCursor(0, 0);
    bytesWritten += lcd.print(date_disp);
    sprintf(date_disp, "%04d-%02d-%02d", nextDt->year, nextDt->month, nextDt->day);
    lcd.setCursor(0, 1);
    bytesWritten += lcd.print(date_disp);

    // Show Julian day of year
    lcd.setCursor(0, 2);
    char doy_disp[9];
    sprintf(doy_disp, "%04d/%03d", currentDt->year, currentDt->dayOfYear);
    bytesWritten += lcd.print(doy_disp);
  }

  if (currentDtValid) {
    if (blinkState) {
      colon = ':';
      blinkState = !blinkState;
    } else {
      colon = '.';
      blinkState = !blinkState;
    }
  }
  
  if (bytesWritten > 0)
    return true;
  else
    return false;
}

