The Jandy JXi/LXi RS-485 protocol
=================================

Packet structure
----------------

The Jandy JXi and LXi pool heaters use the aqualink protocol, at 9600 baud.
A payload is surrounded by a two-byte header, one-byte checksum, and two-byte
footer:

	10 02 [payload] csum 10 03

The checksum is the modulo 256 sum of the bytes in the header and payload.
Payload and checksum bytes of `10` are escaped as `10 00` on the wire.


Payload structure
-----------------

Multi-byte values are little-endian on wire. For example `3b 01`, is 0x13b, or
315.

#### Probe ####

| Field | Size | Description |
| ------| -----|-------------------------------------- |
| dest  |  1   | `68` (to JXi heater)                  |
| cmd   |  1   | `00` - Probe                          |
| arg   |  1   | Unknown - only `00` seen in the wild  |


#### Acknowledge ####

| Field | Size | Description |
| ------| -----|-------------------------------------- |
| dest  |  1   | `00` (from JXi heater)                |
| cmd   |  1   | `01` - Acknowlegde probe packet       |
| arg   |  2   | Unknown - only `00 00` observed       |


#### Control ####

| Field     | Size | Description |
| ----------| -----|----------------------------------------- |
| dest      |  1   | `68` (to JXi heater)                     |
| cmd       |  1   | `0c` - Do stuff                          |
| flags     |  1   | Command bitfield                         |
| temp_pool |  1   | Pool temperature setpoint                |
| temp_spa  |  1   | Spa temperature setpoint                 |
| temp_ext  |  1   | External temperature measurement of `ff` |

Meaning of the command flags:
 - `01` Enable Pool mode
 - `02` Enable Spa mode
 - `04` Temperatures use Celsius scale instead of Fahrenheit
 - `08` Enable heater
 - `10` External temperature byte has valid measurement

For the heater to turn on, both the "enable heater" and one of the "pool" or
"spa" its must be set. If both "pool" and "spa" bits are set, the heater does
not turn on.

The temperature values are encoded so that `e0` -> `ff` represent a two's
complement encoding of a negative number. Values `00` -> `df` are positive
values. For example `ec` is -20 degrees, while `dc` is 220 degrees.

#### Control response ####

| Field  | Size | Description |
| -------| -----|-------------------------------------- |
| dest   |  1   | `68` (to JXi heater)                  |
| cmd    |  1   | `0d` - Status                         |
| status |  1   | Status flags                          |
| args   |  1   | Unkown                                |
| error  |  1   | Error flags                           |

Meaning of status flags:
 - `10` Remote RS-485 is disabled at the panel
 - `08` Heater is on or in the process of igniting

Meaning of error flags:
 - `08` Not burning, no gas, or heater malfunction

#### Get measurements ####

| Field  | Size | Description |
| -------| -----|-------------------------------------- |
| dest   |  1   | `68` (to JXi heater)                  |
| cmd    |  1   | `25` - Get measurements               |


| Field      | Size | Description |
| -----------| -----|--------------------------------------- |
| dest       |  1   | `00` (from JXi heater)                 |
| cmd        |  1   | `25` - Measurements                    |
| gv_on_time |  2   | Lifetime gas valve on time (in hours)  |
| cycles     |  2   | Lifetime number of ignition cycles     |
| last_fault |  1   | `00` - no fault; `f5` - check ignition |
| prev_fault |  1   |                                        |
| temp       |  1   | Water temperature reading + 20         |

The temperature measurement is 20 above the actual reading. The temperature
unit depends "Temperature Scale" set at the control panel. For example, a raw
reading of 100 (0xa0) means a temperature of 80 °F or °C. It is not clear how
to determine the temperature scale from RS-485.

The fault codes are cleared when reading them from the service menu of the
heater's control panel.

Example packets
---------------

Get measurements (arg = 0)

	-> 10 02 68 25 00 9f 10 03
	<- 10 02 00 25 12 00 3b 01 00 00 20 a5 10 03

Reply:
 - `12 00` gv_on_time = 18 hours (0x0012)
 - `3b 01` cycles = 315 (0x013b)
 - `00 00` no faults
 - `20` temp_raw=32 (12 degrees, probably °C)
