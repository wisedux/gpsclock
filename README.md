# gpsclock
A GPS-synced Arduino that sends IRIG-B time signals for a big LED clock 
display.  A standard LCD can be connected since you'll invariably need 
to troubleshoot the IRIG-B signal.

I wrote this so I could use a timecode display [like this](https://prostudioconnection.com/products/symmetricom-820-210-rd-1-irig-b-serial-timecode-red-led-remote-display-rackmount-refurbished) 
I was lucky enough to find in a dumpster somewhere.

## Important Note

**This functions, but not well enough to use seriously.**

Decoding serial signals from the GPS, as they come in, while trying to 
output the IRIG-B signal appears to be too much for my Arduino.  Since 
the IRIG-B signal is active pretty much all the time, sending bits or 
markers every 100 ms, it can be tough keeping up with the serial input 
while not missing the next IRIG output.

I would like to re-write this so that inputs and outputs can be handled 
in parallel.  Two Arduinos could do this.  Alternatively, it could run 
on more beefier hardware.

## Useful Resources

* [Adafruit GPS Library](https://github.com/adafruit/Adafruit_GPS):
  Read the example code!

* joshuaferrara's [IRIG-Arduino decoder](https://github.com/joshuaferrara/IRIG-Arduino) 
  to validate the IRIG-B signal you send

* Cyber Sciences [IRIG-B technical note](http://www.cyber-sciences.com/wp-content/uploads/2019/01/TN-102_IRIG-B.pdf) 
  which describes the IRIG-B signal more simply than anything else I found

* Meinberg [IRIG Time Code Formats](https://www.meinbergglobal.com/english/info/irig.htm)
  which goes into deep detail on the surprisingly diverse IRIG formats

* Wikipedia's [IRIG timecode](https://en.wikipedia.org/wiki/IRIG_timecode)
  article has a decent table breaking down each IRIG bit's meaning

## Prerequisites
* The [Arduino IDE](https://www.arduino.cc/en/Main/Software)

* [Adafruit GPS](https://github.com/adafruit/Adafruit_GPS) library

* [digitalwritefast](https://code.google.com/archive/p/digitalwritefast/) library

  *Note, this library is quite old.  You will have to modify it as follows:*
  ```
  Need to change #include "WProgram.h" to #include "Arduino.h"
  and #include <wiring.h> to #include <wiring_private.h>
  ```

## Bill of Materials
### Required
1 Arduino UNO compatible board (I used the 
[SparkFun Redboard](https://www.sparkfun.com/products/13975), which I got 
for free from some promotion several years back.)

1 [Adafruit Ultimate GPS Breakout](https://www.adafruit.com/product/746)

1 50 Ohm BNC Jack, e.g. [TE Connectivity 5227161-7](https://www.digikey.com/product-detail/en/te-connectivity-amp-connectors/5227161-7/A32260-ND/811158)

1 50 Ohm BNC Cable, e.g. [Steren Electronics 36" RG56/U](https://www.jameco.com/webapp/wcs/stores/servlet/ProductDisplay?langId=-1&storeId=10001&catalogId=10001&productId=11404)

1 10k 1/4 watt pulldown resistor

1 Breadboard

Wire

### Optional, but recommended
1 20x4 LCD, Hitachi HD44780 compatible.  I used an old Hantronix 
[HDM20416H](https://www.mouser.com/ProductDetail/Hantronix/HDM20416H-S00S?qs=NSR9MB9QqxYnIDs7uTmqSQ%3D%3D)

1 [external GPS antenna](https://www.adafruit.com/product/960) Place the 
antenna by a window: GPS doesn't work well indoors

1 [SMA to μFL adapter](https://www.adafruit.com/product/851) Required if 
using external antenna (above)

An oscilloscope or logic probe.  You can't troubleshoot this without one.  I 
used and recommend Diglent's 
[Analog Discovery 2](https://store.digilentinc.com/analog-discovery-2-100msps-usb-oscilloscope-logic-analyzer-and-variable-power-supply/).

A second Arduino UNO compatible board to run joshuaferrara's 
[IRIG-Arduino decoder](https://github.com/joshuaferrara/IRIG-Arduino)

## Hardware Setup
**TO DO: Schematics**

### LCD (optional, but recommended)
| Arduino Pin | LCD pin |
|------------:|:--------|
|          12 | RS      |
|          11 | EN      |
|           7 | D4      |
|           6 | D5      |
|           5 | D6      |
|           4 | D7      |

### GPS module
| Arduino Pin | GPS pin |
|------------:|:--------|
|           0 | TX      |
|           1 | RX      |
|           2 | PPS     |

Attach a 10k pulldown resister from PPS to ground.

*Optional:* attach external antenna and place it by a window.

### IRIG-B output
| Arduino Pin | Purpose    |
|------------:|:-----------|
|          10 | IRIG-B out |

Connect IRIG-B out and ground to the BNC connector.

## Troubleshooting
Use a spare Arduino to verify the GPS is working normally.  Follow the 
[instructions for the module](https://learn.adafruit.com/adafruit-ultimate-gps) 
over at Adafruit.  You should see NMEA sentences streaming from the unit, 
and they should make sense from a lat/long and time perspective.

Verify the IRIG-B signal looks decent from a scope.

Once the IRIG-B signal looks reasonable, I suggest connecting a second 
Arduino running joshuaferrara's [IRIG-Arduino decoder](https://github.com/joshuaferrara/IRIG-Arduino) 
to the IRIG-B output, to validate the signal.

If you got this far, and are wondering why this is flaky as all frig, read 
the important note up top!  Reading serial and outputting IRIG-B is too 
much for one mortal Arduino to handle.  Someday I will redesign this so it 
works stably, but that day has not yet come.
