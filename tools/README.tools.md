aqualink-control development tools
==================================

The `tools` directory contains a seris of scripts and tools that are intended
for development, but are not part of the final aqualink-control program.

aquaproto.py
------------

`aquaproto` is a basic implementation of the aqualink protocol encoding and
transmission. It is also a standalone tool that monitors an RS-485 bus for
aqualink pakets.

jxi_heater_control.py
---------------------

`jxi_heater_control` is an experimental implementation of an aqualink RS-485
bus master for Jandy JXi heaters. It is an incomplete aqualink implementation,
as it only communicates to heates with device id `0x68`. It also doubles as a
protocol decoder when run in `--monitor-only` mode.

The heater can be controlled via a socket interface. The protocol is text based
such that it may be used with echo and netcat. To get a list of commands:

    $ echo help | nc -U /tmp/aquaheat.sock

### Heater on/off control

    $ echo heater on spa | nc -U /tmp/aquaheat.sock

The JXi protocol has fields for temperature setpoints, which the heater appears
to ignore. In RS-485 mode, the heater continues to heat past any setpoints,
irrespective of the reading from the internal temperature probe.
`jxi_heater_control` thus implements a five-minute timeout that can be reset by
periodically sending the "on" command.

Specifying 'spa' or 'pool' turns on the respective LED on the heater's control
panel. If neither are given, the heater may not turn on, but otherwise the
specific mode does not affect functioning over RS-485.

### Temperature control

The "measurements" query reports a temperature number. However, the units
(C of F) is set in the control panel's Service Menu. The units setting cannot
be queried via RS-485. This makes the reported temperature an unsuitable metric
for a temperature control loop. `jxi_heater_control` does not implement any
temperature regulation logic.

Temperature control necessitates an external temperature probe, and is thus
beyond the scope of `jxi_heater_control`.
