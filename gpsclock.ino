
#include <LiquidCrystal.h>

// From https://github.com/adafruit/Adafruit_GPS
#include <Adafruit_GPS.h>

//#include <SoftwareSerial.h>

// Interrupt-safe digitalWriteFast from https://code.google.com/archive/p/digitalwritefast/
// Need to change #include "WProgram.h" to #include "Arduino.h"
//            and #include <wiring.h> to #include <wiring_private.h>
#include <digitalWriteFast.h>

// LCD pin assignments
// redboard
const byte rs = 12, en = 11, d4 = 7, d5 = 6, d6 = 5, d7 = 4;
// pro trinket
//const byte rs = 8, en = 6, d4 = 10, d5 = 11, d6 = 12, d7 = 13;
LiquidCrystal lcd(rs, en, d4, d5, d6, d7);

// 1PPS (1 pulse per second) pin on GPS module
// has 10k pulldown resistor
// redboard
const byte PPS_pin = 2;
// pro trinket
//const byte PPS_pin = 3;

/*
   Set GPS serial pins
   Can't use SoftwareSerial if we need to be picky with timer interrupts
   (interrupts are disabled during TX/RX)
*/
// GPS TX pin 8; GPS RX pin 9
//SoftwareSerial GPSSerial(8, 9);
// GPS TX pin 0; GPS RX pin 1
#define GPSSerial Serial

Adafruit_GPS GPS(&GPSSerial);

// Assume year is 2000..2099 (NMEA only provides 2-digit year)
// The ghost of Y3K future haunts me!
const byte century = 20;

// Send IRIG signal out this pin
// redboard
const byte IRIG_pin = 10;
// pro trinket
//const byte IRIG_pin = 4;

// Timing macros (in units of timer1 ticks)
// For 20 kHz:
#define SERIAL_BYTE 20  // = 1 ms (20.83 ticks per 1.0417 ms [1 char @ 9600 baud])
                        // is not critical, since hardware serial is apparently buffered
#define FUDGE         3 // when sending IRIG data, consider it aligned if (ticks % 0 <= FUDGE)
#define IRIG_LOW    40  // = 2 ms
#define IRIG_HIGH   100 // = 5 ms
#define IRIG_MARKER 160 // = 8 ms
#define IRIG_BIT    200 // = 10 ms
//#define UPDATE_LCD    2 // = asap
// Update LCD during IRIG bits that are always low
#define UPDATE_LCD_TIME_AT 1050 //  52.5 ms
#define UPDATE_LCD_DATE_AT 3650 // 182.5 ms
//#define PARSE_NMEA  14020 // = 701 ms
#define PARSE_NMEA  15020 // = 751 ms
//#define PARSE_NMEA  8000 // = 400 ms
#define FULL_SECOND 20000
// In units of milliseconds
#define IRIG_LOW_MS    2
#define IRIG_HIGH_MS   5
#define IRIG_MARKER_MS 8

volatile byte updateLCD = 0;  // Bitmask telling main loop to update the LCD
// OR these bits together to update various strings on LCD
#define LCD_UPDATE_DATE B00000001
#define LCD_UPDATE_TIME B00000010

struct irig_datetime {
  byte second, minute, hour;
  byte day;         // last 2 digits only
  byte month, year; // not used by IRIG-B
  unsigned int dayOfYear;
  unsigned long sbs; // straight binary seconds (17-bit number)
};

// Two rotating datetime structs; one is set after NMEA parsed
irig_datetime dt1;
irig_datetime dt2;

// Pointers for next (written) and current (read) datetime
volatile irig_datetime *nextDt;
volatile irig_datetime *currentDt;

volatile bool clockValid = false; // Prevent displaying 2000-00-00 00:00:01 at startup
volatile bool parseNMEA = false;  // Flag telling main loop to begin parsing NMEA sentence
bool NMEAparsed = false;          // Flag telling main loop NMEA sentence is done being parsed
volatile unsigned int ticks = 0;  // Number of timer1 interrupts since 1PPS pulse
bool blinkState = true; // Toggles each second: used for blinking stuff on LCD

void setup() {
  // IRIG pin is output
  pinMode(IRIG_pin, OUTPUT);
  /*
     Configure TIMER1 for 20kHz rate
  */
  cli(); // stop interrupts
  TCCR1A = 0; // set entire TCCR0A register to 0
  TCCR1B = 0; // same for TCCR0B
  TCNT1  = 0; // initialize counter value to 0
  // set compare match register
  OCR1A = 800; // = (16e6 / (1 * 20000)) - 1 + fudge factor
  // turn on CTC mode
  TCCR1B |= (1 << WGM12);
  // 1x prescaler
  TCCR1B |= (1 << CS10);
  // enable timer compare interrupt
  TIMSK1 |= (1 << OCIE1A);
  sei(); // enable interrupts

  // 9600 NMEA is the default baud rate for Adafruit MTK GPS's- some use 4800
  GPS.begin(9600);

  //GPS.sendCommand(PMTK_SET_BAUD_9600);
  //GPS.sendCommand(PMTK_SET_BAUD_57600);
  //GPSSerial.println("$PMTK251,57600*2C");
  //GPSSerial.println("$PMTK251,115200*1F");
  //GPSSerial.end();
  //GPS.begin(9600);
  delay(500);

  // Have GPS send no NMEA strings at beginning
  //GPS.sendCommand(PMTK_SET_NMEA_OUTPUT_OFF);
  //GPS.sendCommand(PMTK_SET_NMEA_OUTPUT_RMCONLY);
  // ACK
  //GPS.waitForSentence("PMTK001,", 20);

  // Set the update rate
  // Inexplicably, at 1Hz or 2Hz update rate, 1/4 of sentences fail to be read
  //GPS.sendCommand(PMTK_SET_NMEA_UPDATE_5HZ);   // 5 Hz update rate
  // ACK (code will hang here if GPS module is unresponsive)
  //GPS.waitForSentence("PMTK001,", 20);

  // Ask for firmware version
  //GPSSerial.println(PMTK_Q_RELEASE);
  //GPS.waitForSentence("PMTK705,", 80);

  // set up the LCD's number of columns and rows:
  lcd.begin(20, 4);

  // Look for pulse on 1PPS pin
  attachInterrupt(digitalPinToInterrupt(PPS_pin), ppsInterrupt, RISING);
}

// This is super busy for an ISR, but it ensures frame markers and 
// position identifiers are accurate
ISR(TIMER1_COMPA_vect) {
  if (ticks >= FULL_SECOND)
    return;
  
  ticks++;

  // Read a character over serial if available
  // Can't get this to work in main loop
  if (ticks > IRIG_BIT && ticks % SERIAL_BYTE == 0)
    GPS.read();

  switch (ticks) {
    case PARSE_NMEA:
      parseNMEA = true;
      break;
    case UPDATE_LCD_DATE_AT:
      updateLCD |= LCD_UPDATE_DATE;
      break;
    case UPDATE_LCD_TIME_AT:
      updateLCD |= LCD_UPDATE_TIME;
      break;
    // Frame marker end (brought high during 1PPS ISR)
    case IRIG_MARKER:
      digitalWriteFast(IRIG_pin, LOW);
      break;
  }
}

// ISR to
// * send IRIG start-of-second marker when 1PPS pin goes high
// * reset variables to start of second conditions
void ppsInterrupt () {
  digitalWriteFast(IRIG_pin, HIGH);
  ticks = 0;
  // If NMEA sentence couldn't be parsed last second it will start out true
  parseNMEA = false;
  // Swap current/next datetime pointers
  if (currentDt == &dt1) {
    currentDt = &dt2;
    nextDt = &dt1;
  } else {
    currentDt = &dt1;
    nextDt = &dt2;
  }
}

static bool captureTime() {
  if (NMEAparsed) {
    // Add 1 second to time since it's already out of date
    nextDt->second = GPS.seconds + 1;
    if (nextDt->second <= 59) {
      nextDt->minute = GPS.minute;
      nextDt->hour = GPS.hour;
      nextDt->day = GPS.day;
      nextDt->month = GPS.month;
      nextDt->year = GPS.year;
    } else {
      nextDt->second = 0;
      nextDt->minute = GPS.minute + 1;
      if (nextDt->minute <= 59) {
        nextDt->hour = GPS.hour;
        nextDt->day = GPS.day;
        nextDt->month = GPS.month;
        nextDt->year = GPS.year;
      } else {
        nextDt->minute = 0;
        nextDt->hour = GPS.hour + 1;
        if (GPS.hour <= 23) {
          nextDt->day = GPS.day;
          nextDt->month = GPS.month;
          nextDt->year = GPS.year;
        } else {
          nextDt->hour = 0;
          nextDt->day = GPS.day + 1;
          // Forget about month and year rollover for now.
          nextDt->month = GPS.month;
          nextDt->year = GPS.year;
        }
      }
    }

    // Calculate straight binary seconds
    nextDt->sbs = nextDt->hour * 3600 + nextDt->minute * 60 + nextDt->second;

    // Calculate day of year
    // If March or later, we need to know whether it's a leap year
    byte leapYear;
    if (nextDt->month > 2) {
      unsigned int fullYear = century * 100 + nextDt->year;
      if (fullYear % 4 != 0)
        leapYear = 0;
      else if (fullYear % 100 != 0)
        leapYear = 1;
      else if (fullYear % 400 != 0)
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
    return true;
  } else {
    // Clock not valid - no time to set
    return false;
  }
}


//static bool doLCDupdate (byte whatToUpdate) {
static bool doLCDupdate () {
  //char datetime_disp[21]; // buffer to hold formatted date and time
  char colon;             // HH:MM:SS separator
  int bytesWritten;


  if (updateLCD & LCD_UPDATE_TIME) {
    char time_disp[10];
    if (clockValid)
      sprintf(time_disp, " %02d:%02d:%02d", currentDt->hour, currentDt->minute, currentDt->second);
    else
      sprintf(time_disp, " XX:XX:XX");
    lcd.setCursor(10, 0);
    bytesWritten += lcd.print(time_disp);
  }

  if (updateLCD & LCD_UPDATE_DATE) {
    char date_disp[11];
    if (clockValid)
      sprintf(date_disp, "%02d%02d-%02d-%02d", century, currentDt->year, currentDt->month, currentDt->day);
    else
      sprintf(date_disp, "XXXX-XX-XX");
    lcd.setCursor(0, 0);
    bytesWritten += lcd.print(date_disp);
    // Ugly place to do this
    clockValid = false;
  }
  
  /*
  if (clockValid) {
    if (blinkState) {
      colon = ':';
      blinkState = !blinkState;
    } else {
      colon = '.';
      blinkState = !blinkState;
    }
    //sprintf(datetime_disp, "%02d%02d-%02d-%02d %02d%c%02d%c%02d", 
    //  century, currentDt->year, currentDt->month, currentDt->day, 
    //  currentDt->hour, colon, currentDt->minute, colon, currentDt->second);
    sprintf(datetime_disp, "%02d:%02d:%02d ", currentDt->hour, currentDt->minute, currentDt->second);
    //sprintf(datetime_disp, "%02d%02d-%02d-%02d", century, currentDt->year, currentDt->month, currentDt->day);
  } else {
    sprintf(datetime_disp, "   Time not valid   ");
  }

  lcd.setCursor(0, 0);
  int bytesWritten = lcd.print(datetime_disp);*/

  /*char doy_disp[9];
  char sbs_disp[10];
  sprintf(doy_disp, "DOY %03d ", currentDt->dayOfYear);
  sprintf(sbs_disp, "SBS %05d", currentDt->sbs);
  lcd.setCursor(0, 1);
  lcd.print(doy_disp);
  lcd.setCursor(8, 1);
  lcd.print(sbs_disp);*/

  if (bytesWritten > 0)
    return true;
  else
    return false;
}

static void writeIRIG() {
  // Keep track of current bit in IRIG signal
  // Refresh value of currentBit throughout this routine since it drifts enough
  byte currentBit = 0;
  //if (ticks % IRIG_BIT <= FUDGE)
  //  currentBit = ticks / IRIG_BIT;
  
  // Frame marker end (brought high during 1PPS ISR)
  //if (ticks >= IRIG_MARKER && ticks <= IRIG_MARKER + FUDGE)
  //    digitalWriteFast(IRIG_pin, LOW);

  // Seconds
  if (ticks >= 1*IRIG_BIT && ticks <= 9*IRIG_BIT + FUDGE) {
    byte second_tens = currentDt->second / 10;
    byte second_ones = currentDt->second % 10;
    if (ticks % IRIG_BIT <= FUDGE) {
      currentBit = ticks / IRIG_BIT;
      digitalWriteFast(IRIG_pin, HIGH);
      switch (currentBit) {
        case 1:
        case 2:
        case 3:
        case 4:
          if (((1 << (currentBit-1)) & second_ones) != 0)
            delay(IRIG_HIGH_MS);
          else
            delay(IRIG_LOW_MS);
          break;
        case 5: // is always low
          delay(IRIG_LOW_MS);
          break;
        case 6:
        case 7:
        case 8:
          if (((1 << (currentBit-6)) & second_tens) != 0)
            delay(IRIG_HIGH_MS);
          else
            delay(IRIG_LOW_MS);
          break;
        case 9:
          delay(IRIG_MARKER_MS);
          break;
      }
      digitalWriteFast(IRIG_pin, LOW);
    }
  
  // Minutes
  } else if (ticks >= 10*IRIG_BIT && ticks <= 19*IRIG_BIT + FUDGE) {
    byte minute_tens = currentDt->minute / 10;
    byte minute_ones = currentDt->minute % 10;
    if (ticks % IRIG_BIT <= FUDGE) {
      currentBit = ticks / IRIG_BIT;
      digitalWriteFast(IRIG_pin, HIGH);
      switch (currentBit) {
        case 10:
        case 11:
        case 12:
        case 13:
          if (((1 << (currentBit-10)) & minute_ones) != 0)
            delay(IRIG_HIGH_MS);
          else
            delay(IRIG_LOW_MS);
          break;
        case 14: // is always low
          delay(IRIG_LOW_MS);
          break;
        case 15:
        case 16:
        case 17:
          if (((1 << (currentBit-15)) & minute_tens) != 0)
            delay(IRIG_HIGH_MS);
          else
            delay(IRIG_LOW_MS);
          break;
        case 18: // is always low
          delay(IRIG_LOW_MS);
          break;
        case 19:
          delay(IRIG_MARKER_MS);
          break;
      }
      digitalWriteFast(IRIG_pin, LOW);
    }
  
  // Hours
  } else if (ticks >= 20*IRIG_BIT && ticks <= 29*IRIG_BIT + FUDGE) {
    byte hour_tens = currentDt->hour / 10;
    byte hour_ones = currentDt->hour % 10;
    if (ticks % IRIG_BIT <= FUDGE) {
      currentBit = ticks / IRIG_BIT;
      digitalWriteFast(IRIG_pin, HIGH);
      switch (currentBit) {
        case 20:
        case 21:
        case 22:
        case 23:
          if (((1 << (currentBit-20)) & hour_ones) != 0)
            delay(IRIG_HIGH_MS);
          else
            delay(IRIG_LOW_MS);
          break;
        case 24: // is always low
          delay(IRIG_LOW_MS);
          break;
        case 25:
        case 26:
          if (((1 << (currentBit-25)) & hour_tens) != 0)
            delay(IRIG_HIGH_MS);
          else
            delay(IRIG_LOW_MS);
          break;
        case 27: // is always low
        case 28: // is always low
          delay(IRIG_LOW_MS);
          break;
        case 29:
          delay(IRIG_MARKER_MS);
          break;
      }
      digitalWriteFast(IRIG_pin, LOW);
    }

  // Day of year (ones and tens)
  } else if (ticks >= 30*IRIG_BIT && ticks <= 39*IRIG_BIT + FUDGE) {
    byte doy_tens = (currentDt->dayOfYear / 10) % 10;
    byte doy_ones = currentDt->dayOfYear % 10;
    if (ticks % IRIG_BIT <= FUDGE) {
      currentBit = ticks / IRIG_BIT;
      digitalWriteFast(IRIG_pin, HIGH);
      switch (currentBit) {
        case 30:
        case 31:
        case 32:
        case 33:
          if (((1 << (currentBit-30)) & doy_ones) != 0)
            delay(IRIG_HIGH_MS);
          else
            delay(IRIG_LOW_MS);
          break;
        case 34: // is always low
          delay(IRIG_LOW_MS);
          break;
        case 35:
        case 36:
        case 37:
        case 38:
          if (((1 << (currentBit-35)) & doy_tens) != 0)
            delay(IRIG_HIGH_MS);
          else
            delay(IRIG_LOW_MS);
          break;
        case 39:
          delay(IRIG_MARKER_MS);
          break;
      }
      digitalWriteFast(IRIG_pin, LOW);
    }
  
  // Day of year (hundreds) [bits 45-48 are tenths of seconds - not part of IRIG-B]
  } else if (ticks >= 40*IRIG_BIT && ticks <= 49*IRIG_BIT + FUDGE) {
    byte doy_hundreds = (currentDt->dayOfYear / 100);
    if (ticks % IRIG_BIT <= FUDGE) {
      currentBit = ticks / IRIG_BIT;
      digitalWriteFast(IRIG_pin, HIGH);
      switch (currentBit) {
        case 40:
        case 41:
          if (((1 << (currentBit-40)) & doy_hundreds) != 0)
            delay(IRIG_HIGH_MS);
          else
            delay(IRIG_LOW_MS);
          break;
        case 42: // is always low
        case 43: // is always low
        case 44: // is always low
        case 45: // is always low [in IRIG-B]
        case 46: // is always low [in IRIG-B]
        case 47: // is always low [in IRIG-B]
        case 48: // is always low [in IRIG-B]
          delay(IRIG_LOW_MS);
          break;
        case 49:
          delay(IRIG_MARKER_MS);
          break;
      }
      digitalWriteFast(IRIG_pin, LOW);
    }

  // Year (last two digits only)
  } else if (ticks >= 50*IRIG_BIT && ticks <= 59*IRIG_BIT + FUDGE) {
    byte year_tens = currentDt->year / 10;
    byte year_ones = currentDt->year % 10;
    if (ticks % IRIG_BIT <= FUDGE) {
      currentBit = ticks / IRIG_BIT;
      digitalWriteFast(IRIG_pin, HIGH);
      switch (currentBit) {
        case 50:
        case 51:
        case 52:
        case 53:
          if (((1 << (currentBit-50)) & year_ones) != 0)
            delay(IRIG_HIGH_MS);
          else
            delay(IRIG_LOW_MS);
          break;
        case 54: // is always low
          delay(IRIG_LOW_MS);
          break;
        case 55:
        case 56:
        case 57:
        case 58:
          if (((1 << (currentBit-55)) & year_tens) != 0)
            delay(IRIG_HIGH_MS);
          else
            delay(IRIG_LOW_MS);
          break;
        case 59:
          delay(IRIG_MARKER_MS);
          break;
      }
      digitalWriteFast(IRIG_pin, LOW);
    }
  
  // Control functions 1 & 2: all bits are LOW
    } else if ((ticks >= 60*IRIG_BIT && ticks <= 79*IRIG_BIT + FUDGE)) {
      if (ticks % IRIG_BIT <= FUDGE) {
        currentBit = ticks / IRIG_BIT;
        digitalWriteFast(IRIG_pin, HIGH);
        switch (currentBit) {
          case 69:
          case 79:
            delay(IRIG_MARKER_MS);
            break;
          default:
            delay(IRIG_LOW_MS);
            break;
        }
        digitalWriteFast(IRIG_pin, LOW);
      }

  // Straight binary seconds (least significant 9 bits)
  } else if (ticks >= 80*IRIG_BIT && ticks <= 89*IRIG_BIT + FUDGE) {
    unsigned int sbs_lsb9 = (unsigned int) currentDt->sbs & 511; // 511 is 9 ones
    if (ticks % IRIG_BIT <= FUDGE) {
      currentBit = ticks / IRIG_BIT;
      digitalWriteFast(IRIG_pin, HIGH);
      switch (currentBit) {
        case 80:
        case 81:
        case 82:
        case 83:
        case 84:
        case 85:
        case 86:
        case 87:
        case 88:
          if ((((unsigned int) 1 << (currentBit-80)) & sbs_lsb9) != 0)
            delay(IRIG_HIGH_MS);
          else
            delay(IRIG_LOW_MS);
          break;
        case 89:
          delay(IRIG_MARKER_MS);
          break;
      }
      digitalWriteFast(IRIG_pin, LOW);
    }

  // Straight binary seconds (most significant 8 bits)
  } else if (ticks >= 90*IRIG_BIT && ticks <= 99*IRIG_BIT + FUDGE) {
    byte sbs_msb8 = (byte) currentDt->sbs >> 9;
    if (ticks % IRIG_BIT <= FUDGE) {
      currentBit = ticks / IRIG_BIT;
      digitalWriteFast(IRIG_pin, HIGH);
      switch (currentBit) {
        case 90:
        case 91:
        case 92:
        case 93:
        case 94:
        case 95:
        case 96:
        case 97:
        case 98:
          if (((1 << (currentBit-90)) & sbs_msb8) != 0)
            delay(IRIG_HIGH_MS);
          else
            delay(IRIG_LOW_MS);
          break;
        case 99:
          delay(IRIG_MARKER_MS);
          break;
      }
      digitalWriteFast(IRIG_pin, LOW);
    }
  }
  
}

void loop() {

  // FIXME: might not want fudge here
  //if (ticks >= PARSE_NMEA && ticks <= PARSE_NMEA + FUDGE) {
  if (parseNMEA) {
    if (GPS.newNMEAreceived())
      NMEAparsed = GPS.parse(GPS.lastNMEA());
    if (NMEAparsed) {
      // Stores value of next second into globals (second, minute, hour, dayOfYear, year, sbs)
      clockValid = captureTime();
      NMEAparsed = false;
    }
  }

  // FIXME: might not want fudge here
  //if (ticks >= UPDATE_LCD && ticks <= UPDATE_LCD + FUDGE) 
  //    updateLCD = true;
    
  if (updateLCD) {
    if (doLCDupdate());
      updateLCD = 0;
  }

  if (ticks < FULL_SECOND)
    writeIRIG();

}




