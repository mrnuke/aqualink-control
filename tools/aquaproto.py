#! /bin/env python3
'''Implementation of the Aqualink RS-485 protocol

This module implements the encoding of RS-485 packets as used by Jandy pool
equipment, commonly marketed as "Aqualink". When ran as a program, it monitors
an RS-485 bus and prints any aqualink packets.
'''

import asyncio
import datetime
from sys import argv

#pylint: disable=import-error
import serial_asyncio

class AqualinkProtocol(asyncio.Protocol):
    '''Aqualink RS-485 protocol: chunk aqualink stream data into packets'''
    def __init__(self):
        self.escape_seq = bytes([0x10, 0x00])
        self.header = bytes([0x10, 0x02])
        self.footer = bytes([0x10, 0x03])
        self.input_buf = bytearray()
        self.transport = None
        self.closed = False
        super().__init__()

    def connection_made(self, transport):
        self.transport = transport

    def data_received(self, data):
        self.input_buf.extend(data)

    def __read(self, n_bytes):
        read_bytes = self.input_buf[:n_bytes]
        del self.input_buf[:n_bytes]

        return read_bytes

    def __packet_search(self):
        while len(self.input_buf) > len(self.header):
            pkt_start_idx = self.input_buf.find(self.header)

            if pkt_start_idx < 0:
                # No header was found, so all the bytes are junk. Discard them
                pkt_start_idx = len(self.input_buf)

            if pkt_start_idx > 0:
                dropped_bytes = self.__read(pkt_start_idx)
                print(f'Dropping bytes {dropped_bytes.hex(" ")}')

            pkt_footer_idx = self.input_buf.find(self.footer)
            if pkt_footer_idx < 0:
                return

            raw_pkt = self.__read(pkt_footer_idx + len(self.footer))
            pkt = raw_pkt.replace(self.escape_seq, bytes([0x10]))
            csum = sum(pkt[:-3]) & 0xff
            if csum != pkt[-3]:
                print(f'Invalid checksum {hex(pkt[-3])}. expected {hex(csum)}; {pkt.hex(" ")}')
                continue

            yield pkt[2:-3]

    def read_packet(self):
        '''Return a generator that spits out complete packets'''
        return self.__packet_search()

    def write_packet(self, pkt):
        '''Write a packet. Encoding and checksumming is automatically handled'''
        csum = (sum(self.header) + sum(pkt)) & 0xff
        # Payload and checksum bytes must be escaped if 0x10
        rs485_pkt = (pkt + bytes([csum])).replace(bytes([0x10]), self.escape_seq)
        rs485_pkt = self.header + rs485_pkt + self.footer
        self.transport.write(rs485_pkt)


async def main():
    '''Basic example of AqualinkProtocol class usage'''
    tty_path = argv[1] if len(argv) > 1 else '/dev/ttyS0'

    loop = asyncio.get_event_loop()
    _, protocol = await serial_asyncio.create_serial_connection(loop,
                                    AqualinkProtocol,
                                    tty_path, baudrate=9600)
    await asyncio.sleep(0.3)
    while not protocol.closed:
        await asyncio.sleep(0.010)
        for pkt in protocol.read_packet():
            now = datetime.datetime.now()
            print(f'[{now}] {pkt.hex(" ")}')

if __name__ == "__main__":
    asyncio.run(main())
