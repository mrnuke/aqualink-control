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
