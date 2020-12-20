/*
 * GPS Clock Sync
 * 
 * Part 2 of 2 for a GPS driven IRIG-B wall clock.
 * 
 * - I2C master that retrieves next second's time data from GPS Clock Syncer
 * - Outputs IRIG-B signal
 */

#include <Wire.h>
// Interrupt-safe digitalWriteFast from https://code.google.com/archive/p/digitalwritefast/
// Need to change #include "WProgram.h" to #include "Arduino.h"
//            and #include <wiring.h> to #include <wiring_private.h>
#include <digitalWriteFast.h>

#define I2C_SLAVE_ADDR 9

// 1PPS (1 pulse per second) pin on GPS module
// has 10k pulldown resistor
// redboard
const byte PPS_pin = 2;
// pro trinket
//const byte PPS_pin = 3;

// Send DC level shifted IRIG signal out this pin
// redboard
const byte IRIG_pin = 10;
// pro trinket
//const byte IRIG_pin = 4;

// Send 1 kHz carrier out this pin
const byte carrier_pin = 9;

// Timing macros (in units of timer1 ticks)
// For 20 kHz:
#define CARRIER_HALF 10 // = 500 us (half a carrier cycle)
#define FUDGE         3 // when sending IRIG data, consider it aligned if (ticks % 0 <= FUDGE)
#define IRIG_LOW     40 // = 2 ms
#define IRIG_HIGH   100 // = 5 ms
#define IRIG_MARKER 160 // = 8 ms
#define IRIG_BIT    200 // = 10 ms
//#define UPDATE_LCD   2 // = asap
// Update LCD during IRIG bits that are always low
//#define UPDATE_LCD_TIME_AT 1050 //  52.5 ms
//#define UPDATE_LCD_DATE_AT 3650 // 182.5 ms
//#define PARSE_NMEA  12020 // = 601 ms
//#define PARSE_NMEA  14020 // = 701 ms
//#define PARSE_NMEA  15020 // = 751 ms <--- this
//#define PARSE_NMEA  8000  // = 400 ms
//#define PARSE_NMEA_BEGIN 12020 // = 601 ms
//#define PARSE_NMEA_BEGIN 0
//#define PARSE_NMEA_END   15600 // = 780 ms
#define UPDATE_DATETIME_AT 14020 // = 701 ms
#define FULL_SECOND 20000
// In units of milliseconds
#define IRIG_LOW_MS    2
#define IRIG_HIGH_MS   5
#define IRIG_MARKER_MS 8


typedef struct irig_datetime {
  byte second, minute, hour;
  byte day, month;   // not used by IRIG-B
  unsigned int year;
  unsigned int dayOfYear;
  unsigned long sbs; // straight binary seconds (17-bit number)
};

// Two rotating datetime structs; one is set over I2C
irig_datetime dt1;
irig_datetime dt2;

// Pointers for next (written) and current (read) datetime
volatile irig_datetime *nextDt;
volatile irig_datetime *currentDt;

volatile bool updateDt = false;   // Tells main loop it's time to ask I2C slave for a new datetime

volatile unsigned int ticks = 0;  // Number of timer1 interrupts since 1PPS pulse
volatile byte carrierTicks = 0; // Separate but similar to ticks, but for carrier
volatile bool carrierHigh = true; // Toggles to make carrier high or low

void setup()
{
  // IRIG pin is output
  pinMode(IRIG_pin, OUTPUT);
  pinMode(carrier_pin, OUTPUT);
  digitalWriteFast(carrier_pin, HIGH);
  
  // Configure TIMER1 for 20kHz rate
  cli(); // stop interrupts
  TCCR1A = 0; // set entire TCCR0A register to 0
  TCCR1B = 0; // same for TCCR0B
  TCNT1  = 0; // initialize counter value to 0
  // set compare match register (this will depend on individual board's clock)
  //OCR1A = 800; // = (16e6 / (1 * 20000)) - 1 + fudge factor
  OCR1A = 798; // = (16e6 / (1 * 20000)) - 1 + fudge factor
  // turn on CTC mode
  TCCR1B |= (1 << WGM12);
  // 1x prescaler
  TCCR1B |= (1 << CS10);
  // enable timer compare interrupt
  TIMSK1 |= (1 << OCIE1A);
  sei(); // enable interrupts
  
  Wire.begin();
  pinMode(PPS_pin, INPUT);

  Serial.begin(57600);
  delay(500);
  Serial.println("starting");
  delay(500);

  // Look for pulse on 1PPS pin
  attachInterrupt(digitalPinToInterrupt(PPS_pin), ppsInterrupt, RISING);
}

// This is super busy for an ISR, but it ensures frame markers and 
// position identifiers are accurate
ISR(TIMER1_COMPA_vect) {
  carrierTicks++;
  ticks++;

  if (carrierTicks % CARRIER_HALF == 0) {
    if (carrierHigh)
      digitalWriteFast(carrier_pin, HIGH);
    else
      digitalWriteFast(carrier_pin, LOW);
    carrierHigh = !carrierHigh;
    // Avoid overflow
    //if (carrierTicks >= 65535 - 2*CARRIER_HALF)
    carrierTicks = 0;
  }
  
  if (ticks > FULL_SECOND)
    return;

  switch (ticks) {
    case UPDATE_DATETIME_AT:
      updateDt = true;
      break;
    // Frame marker end (brought high during 1PPS ISR)
    case IRIG_MARKER:
      digitalWriteFast(IRIG_pin, LOW);
      break;
  }
}

// ISR that runs at the top of each second
void ppsInterrupt () {
  // Start of second marker
  digitalWriteFast(IRIG_pin, HIGH);
  ticks = 0;
  
  // Swap current/next datetime pointers
  if (currentDt == &dt1) {
    currentDt = &dt2;
    nextDt = &dt1;
  } else {
    currentDt = &dt1;
    nextDt = &dt2;
  }
}

void loop()
{
  if (updateDt) {
    updateDt = false;
    Wire.requestFrom(I2C_SLAVE_ADDR, sizeof(irig_datetime));
    //cli(); // does this help?
    Wire.readBytes((char *) nextDt, sizeof(irig_datetime));
    //sei();
    Wire.endTransmission();
//    char time_disp[35];
//    sprintf(time_disp, "%04d-%02d-%02d %02d:%02d:%02d %04d/%03d %05lu",
//            nextDt->year, nextDt->month, nextDt->day,
//            nextDt->hour, nextDt->minute, nextDt->second,
//            nextDt->year, nextDt->dayOfYear, nextDt->sbs);
//    Serial.println(time_disp);
  }

  if (ticks < FULL_SECOND)
    writeIRIG();

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
    word sbs_lsb9 = (word) currentDt->sbs & 511; // 511 is 9 ones
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
          if ((((word) 1 << (currentBit-80)) & sbs_lsb9) != 0)
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
    word temp = currentDt->sbs >> 9;
    byte sbs_msb8 = (byte) temp;
    //byte sbs_msb8 = (byte)((word) currentDt->sbs >> 9);
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
