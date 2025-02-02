#! /bin/env python3
'''Master control program for JXi heaters over RS-485

This module implements a control loop for Jandy JXi heaters using an RS-485 bus
and the aqualink protocol. It depends on the aquaproto module.
'''

import argparse
import asyncio
from datetime import datetime
import pprint
import signal

#pylint: disable=import-error
import serial_asyncio
from aquaproto import AqualinkProtocol

JXI_RS485_DEV_ADDR = 0x68
JXI_CTL_POOL = 0x01
JXI_CTL_SPA = 0x02
JXI_CTL_CELSIUS = 0x04
JXI_CTL_HEATER_ON = 0x08
JXI_CTL_TEMP3_VALID = 0x10
# Internal five-minute timeout. Not part of the protocol
JXI_KEEPALIVE_TIMEOUT=300

packet_def = {
    0x00 : { "name" : "probe", "fields" : [ ] },
    0x01 : {
        "name" : "ACK",
        "fields" : [
            ("?_ack1", 1),
            ("?_ack2", 1),
        ]
    },
    0x0c : {
        "name" : "ping",
        "fields" : [
            ("control_flags", 1),
            ("setpoint_spa", 1),
            ("setpoint_pool", 1),
            ("external_temp_reading", 1),
        ],
        "bitfields" : {
            "control_flags" : [
                ("pool", 0x01),
                ("spa", 0x02),
                ("celsius", 0x04),
                ("heater_on", 0x08),
                ("ext_temp_valid", 0x10)
            ]
        }
    },
    0x0d : {
        "name" : "control",
        "fields" : [
            ("status_flags", 1),
            ("?_ctl2", 1),
            ("error_flags", 1),
        ],
        "bitfields" : {
            "status_flags" : [
                ("heater_on", 0x08),
                ("remote_rs485_disabled", 0x10)
            ],
            "error_flags" : [
                ("heater_error", 0x08),
            ]
        }
    },
    0x25 : {
        "name" : "status",
        "fields" : [
            ("gv_on_time", 2),
            ("cycles", 2),
            ("last_fault", 1),
            ("prev_fault", 1),
            ("temp_raw", 1),
        ]
    },
}

def decode_packet(pkt, big_picture):
    '''Decode a JXi packet'''
    dest = pkt[0]
    pdef = packet_def.get(pkt[1])

    if not pdef:
        print(f'**** Unexplained {pkt.hex(" ")} ****')
        return

    offset = 2
    descr = ""
    for name, size in pdef["fields"]:
        if offset + size > len(pkt):
            break

        value = 0
        for idx in reversed(range(offset, offset + size)):
            value = value << 8 | pkt[idx]

        descr += f' {name}={hex(value)}'
        offset += size

        if name.startswith('?'):
            continue

        if 'bitfields' in pdef and name in pdef["bitfields"]:
            for fieldname, mask in pdef["bitfields"][name]:
                big_picture[fieldname] = value & mask
                value &= ~mask

            if value:
                print(f'[{dest:02x}] bitfield "{name}" has unknonwn bits set {value:02x}')

        old_value = big_picture.get(name)
        if old_value and old_value != value:
            now = datetime.now()
            print(f'[{now}] "{name}" changed from {old_value} to {value}')
        big_picture[name] = value


class Heater(AqualinkProtocol):
    '''Extension of Aqualink protocol for JXi heaters'''
    def __init__(self):
        self._status = {}
        self._ctl_byte = JXI_CTL_CELSIUS
        self.setpoint = {
            "pool" : 20,
            "spa" : 35
        }
        self._heater_off_time = None
        super().__init__()

    def _control_packet(self):
        pkt = bytearray([JXI_RS485_DEV_ADDR, 0x0c, 0x00, 0x00, 0x00, 0xff])

        pkt[2] = self._ctl_byte
        pkt[3] = self.setpoint["pool"]
        pkt[4] = self.setpoint["spa"]

        return pkt

    def _format_status(self, _):
        pretty = pprint.PrettyPrinter(indent=4)
        return pretty.pformat(self._status) + '\n'

    def _encode_temperature(self, temp):
        # 0xe0 -> 0xff, negative temperature
        # 0x00 -> 0xdf, positive temperature
        raw_temp = int(temp)
        if raw_temp < -0x20 or raw_temp >= 0xe0:
            raise OverflowError(f'Cannot represent temperature {temp}')

        return raw_temp if raw_temp >= 0 else raw_temp + 0x100

    async def decoder_loop(self, monitor_only):
        '''Decode incoming packets until closed. (asyncio)'''
        while not self.closed:
            await asyncio.sleep(0.3)
            for pkt in self.read_packet():
                decode_packet(pkt, self._status)
                if monitor_only:
                    print(pkt.hex(' '))

        print(self._format_status(None))

    async def prober_loop(self, monitor_only):
        '''Send probe packets until closed. (asyncio)'''
        if monitor_only:
            return

        while not self.closed:
            for cmd in [0x00, 0x0c, 0x25]:
                if cmd == 0x0c:
                    probe_pkt = self._control_packet()
                    decode_packet(probe_pkt, self._status)
                else:
                    probe_pkt = bytes([JXI_RS485_DEV_ADDR, cmd])
                self.write_packet(probe_pkt)

                await asyncio.sleep(1)

    def _parse_setpoint_change(self, verbs):
        mode = verbs.pop(0)
        new_setpoint = verbs.pop(0)

        if not mode in self.setpoint:
            raise ValueError('Use "pool" or "spa" + temp')

        temp = int(new_setpoint[:-1])

        if new_setpoint.endswith('F'):
            temp = (temp - 32) * 5.0 / 9.0
        elif new_setpoint.endswith('C'):
            # Might want to use Fahrenheit scale, as it gives better resolution
            # Right now, Celsius is easier for debugging
            pass
        else:
            raise ValueError('Need to specify "F" or "C" scale with temperature')

        self.setpoint[mode] = self._encode_temperature(temp)

    def _heater_keepalive_timeout(self):
        loop = asyncio.get_running_loop()

        if loop.time() >= self._heater_off_time:
            print("Heater keepalive timed out! Shutting off!")
            self._ctl_byte &= ~JXI_CTL_HEATER_ON
            self._heater_off_time = None
        else:
            # Defer turning off heater
            loop.call_at(self._heater_off_time, self._heater_keepalive_timeout)


    def _main_heater_turn_on(self):
        '''Somebody set up us the gas'''
        old_timeout = self._heater_off_time
        loop = asyncio.get_running_loop()
        self._heater_off_time = loop.time() + JXI_KEEPALIVE_TIMEOUT

        if not old_timeout:
            loop.call_at(self._heater_off_time, self._heater_keepalive_timeout)

        # Preparations are done. Enable the heater on the next control packet
        self._ctl_byte |= JXI_CTL_HEATER_ON

    def _main_heater_turn_off(self):
        '''Somebody cut up us the gas'''
        # Ignore any pending timeouts, as they will just reset themselves
        self._ctl_byte &= ~JXI_CTL_HEATER_ON

    def _parse_heater_mode(self, verbs):
        while verbs:
            verb = verbs.pop(0)
            msg = None

            if verb == "spa":
                self._ctl_byte |= JXI_CTL_POOL
                self._ctl_byte &= ~JXI_CTL_POOL
            elif verb == "pool":
                self._ctl_byte &= ~JXI_CTL_POOL
                self._ctl_byte |= JXI_CTL_POOL
            elif verb == "on":
                self._main_heater_turn_on()
                msg = f"Staring heater. Timeout in {JXI_KEEPALIVE_TIMEOUT} sec."
                msg += ' Re-send "on" command periodically to reset timeout.'
            elif verb == "off":
                self._main_heater_turn_off()
            else:
                raise ValueError(f"Unknown parameter {verb}")

        return msg

    async def sock_client_connected_cb(self, reader, writer):
        '''Somebody wants to talk over the socket interface'''
        writer.write('aquaplay socket interface.\n'.encode())

        sock_cmds = {
            'setpoint' : self._parse_setpoint_change,
            'heater' : self._parse_heater_mode,
            'status' : self._format_status,
            'help' : lambda _ : 'commands: ' + ' '.join(sock_cmds.keys())
        }

        while True:
            line = await reader.readline()
            if not line:
                break

            try:
                verbs = line.decode().split()
                command = verbs.pop(0)

                if command in sock_cmds:
                    reply = sock_cmds[command](verbs)
                    if reply:
                        writer.write(reply.encode())
                else:
                    writer.write(f'unknown command {command}'.encode())

            except (ArithmeticError, ValueError, IndexError) as err:
                writer.write(str(err).encode())

        writer.close()

    def signal_shutdown(self, _, __):
        '''Signal handler to be used with signal.signal()'''
        self.closed = True
        self.transport.close()


async def main(args):
    '''asyncio main. Why asyncio? Why not?'''
    socket_path = '/tmp/aquaheat.sock'
    loop = asyncio.get_event_loop()

    _, heater = await serial_asyncio.create_serial_connection(loop,
                                    Heater, args.tty, baudrate=9600)

    if not args.monitor_only:
        try:
            await asyncio.start_unix_server(heater.sock_client_connected_cb,
                                        path=socket_path)

            print(f'UNIX socket interface at "{socket_path}"')
            print(f'For details, run "echo status | nc -U {socket_path}""')
        except OSError as err:
            print(f'Uh oh! {err}')
            return

    signal.signal(signal.SIGINT, heater.signal_shutdown)

    await asyncio.sleep(0.3)
    await asyncio.gather(
        heater.decoder_loop(args.monitor_only),
        heater.prober_loop(args.monitor_only)
    )

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='JXi heater control')
    parser.add_argument('--monitor-only',
                        help='Only monitor bus. Do not send packets.',
                        action='store_true')
    parser.add_argument('tty', type=str, help='path to serial port',
                        nargs='?', default='/dev/ttyUSB0')
    asyncio.run(main(parser.parse_args()))
