#! /usr/bin/env python3

''' Dot Matrix LCD userspace driver

If using a raspberrypi, I2C needs to be enabled manually:
    # raspi-config nonint do_i2c 0

Or, the kernel driver may be enabled by adding the following to config.txt
    dtoverlay=hd44780-i2c-lcd,display_height=4,display_width=20
'''

import errno
import fcntl
import socket
import struct
import subprocess
import time

#pylint: disable=import-error
import smbus

# HD44780 control signals, as they are mapped on the I2C expander
RS = 0x01
RW = 0x02
EN = 0x04
BL = 0x08

# "function set" command explanation
FN_SET_8BIT_MODE    = 0x10
FN_SET_2_LINES      = 0x08
FN_SET_5X10_DOTS    = 0x04

# "Display on/off" command explanation
DISPLAY_CTL         = 0x08
DISP_CTL_ON         = 0x04
DISP_CTL_CURSOR_ON  = 0x02
DISP_CTL_BLINK_ON   = 0x01


class HD44780Display:
    '''Generic HD44780 LCD template class'''
    def __init__(self, cursor=True, blink=True, font_5x10=False):
        self._cursor_en = cursor
        self._blink_en = blink
        self._is_5x10 = font_5x10

    def _move_cursor(self, row, col):
        raise NotImplementedError

    def _write_burst(self, message):
        raise NotImplementedError

    def redefine_chars(self, loc, data):
        ''' Write CGRAM location. A 5x8 character takes 8 bytes
        For 5 × 8 dots, eight character patterns can be written, and for
        5 × 10 dots, four character patterns can be written.
        '''
        raise NotImplementedError

    def main_screen_turn_on(self):
        '''All your base are belong to us'''
        raise NotImplementedError

    def clear(self):
        '''Clear display'''
        raise NotImplementedError

    def write_line(self, line, message):
        '''For great justice. '''
        self._move_cursor(line, 0)
        self._write_burst(message[:20])


class DotMatrixChardev(HD44780Display):
    '''LCD, controlled by the linux "hd44780" driver'''
    def __init__(self, char_5x10=False):
        super().__init__(cursor=False, blink=False, font_5x10=char_5x10)
        self._lcd_dev = None
        # pylint: disable=consider-using-with
        self._lcd_dev = open('/dev/lcd', 'w', encoding="ascii")

    def __del__(self):
        if self._lcd_dev:
            self._lcd_dev.close()

    def _write_burst(self, message):
        self._lcd_dev.write(message)

    def main_screen_turn_on(self):
        self._lcd_dev.write('\x1b[LI\x1b[L+')

        cursor = 'C' if self._cursor_en else 'c'
        blink = 'B' if self._blink_en else 'b'
        self._lcd_dev.write(f'\x1b[L{cursor}\x1b[L{blink}')

    def redefine_chars(self, loc, data):
        while data:
            char = data[:8]
            data = data[8:]
            self._lcd_dev.write(f'\x1b[LG{loc}{char.hex()};')
            loc += 1

        self._lcd_dev.flush()

    def _reset_cursor(self):
        self._lcd_dev.write('\x1b[H')

    def _move_cursor(self, row, col):
        self._lcd_dev.write(f'\x1b[Ly{row}x{col};')

    def clear(self):
        self._lcd_dev.write('\x1b[2J')


class DotMatrixI2C(HD44780Display):
    '''LCD, with I2C expander. eh!'''
    def __init__(self, i2c_bus, i2c_addr, rows=4, char_5x10=False):
        self._i2c_bus = i2c_bus
        self._i2c_addr = i2c_addr
        self._num_rows = rows

        super().__init__(cursor=False, font_5x10=char_5x10)
        self._i2c = smbus.SMBus(i2c_bus)

    def _write_4bit(self, cmd):
        out = (cmd & 0xf0) | BL
        self._i2c.write_i2c_block_data(self._i2c_addr,
                                       out | EN, [out])

    def _command(self, cmd, bits=0):
        '''Command the HD44780 to do something'''
        bits &= (RS | RW | EN)
        hi4 = (cmd & 0xF0) | bits | BL
        lo4 = (cmd << 4) | bits | BL
        self._i2c.write_i2c_block_data(self._i2c_addr,
                                       hi4 | EN, [hi4,
                                       lo4 | EN,  lo4])

    def _cmd_function_set(self):
        cmd = 0x20
        if self._cursor_en:
            cmd |= DISP_CTL_CURSOR_ON
        if self._blink_en:
            cmd |= DISP_CTL_BLINK_ON

        self._command(cmd)

    def _cmd_display_control(self):
        cmd = 0x08 | DISP_CTL_ON
        if self._num_rows > 2:
            cmd |= FN_SET_2_LINES
        if self._is_5x10:
            cmd |= FN_SET_5X10_DOTS

        self._command(cmd)

    def main_screen_turn_on(self):
        for delay in [0.0041, 0.0001, 0]:
            self._write_4bit(0x30)
            time.sleep(delay)

        # Enter 4 bit mode
        self._write_4bit(0x20)

        # Now all commands need two cycles
        self._cmd_function_set()
        self._cmd_display_control()
        # Auto Increment cursor
        self._command(0x06)
        self.clear()
        self._reset_cursor()

    def _write_burst(self, message):
        '''Write several characters in one transaction

        Organize the I2C messages in a block that the HD44780 sees as a stream
        of commands. The expander changes state after each byte. On a 100KHz I2C
        bus, each byte transmission takes about 90 us. This satisfies the 37us
        execution time of most commands. We can get away with the command
        stream, and reduce the number of I2C transactions.
        '''
        buf = []
        ctl_bits = RS | BL

        while len(message):
            # smbus API supports a maximum of 32 bytes per transaction.
            for byte in message[:8]:
                hi4 = (byte & 0xf0) | ctl_bits
                lo4 = ((byte & 0x0f) << 4) | ctl_bits
                # Data is latched on the faling edge of EN
                buf += [hi4 | EN, hi4, lo4 | EN, lo4]

            self._i2c.write_i2c_block_data(self._i2c_addr, buf[0], buf)
            buf.clear()
            message = message[8:]

    def redefine_chars(self, loc, data):
        addr = loc * 8
        if addr >= 0x40:
            raise OverflowError("Invalid CGRAM location")

        if addr + len(data) > 0x40:
            raise OverflowError("Buffer exceeds CGRAM size")

        self._command(0x40 | loc)
        self._write_burst(data)

    def _reset_cursor(self):
        self._command(0x02)
        time.sleep(1.52E-3)

    def _move_cursor(self, row, col):
        cmd = 0x80

        row_addr = [0, 0x40, 0x14, 0x54]
        addr = row_addr[row] + col
        if addr & 0x80:
            raise OverflowError("Cannot represent so much characters")
        self._command(cmd | addr)

    def clear(self):
        self._command(0x01)

    def write_line(self, line, message):
        super().write_line(line, message[:20].encode('ascii'))


def get_ip_addr(ifname):
    '''self-explanatory (for pylint)'''
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rgs = struct.pack('256s',ifname[:15].encode())
    much_data = fcntl.ioctl(sock.fileno(),0x8915,rgs)
    return socket.inet_ntoa(much_data[20:24])


def get_sys_info():
    '''Stage 1: Gather intelligence'''
    sys = {}
    sys['hostname'] = socket.gethostname()

    for iface in ['eth0', 'wlan0']:
        sys[iface] = {}
        try:
            sys[iface]['ip'] = get_ip_addr(iface)
        except OSError as err:
            # ENODEV indicates that there is no point probing further
            if err.errno ==  errno.ENODEV:
                continue

        with open(f'/sys/class/net/{iface}/carrier', encoding='utf-8') as rdr:
            sys[iface]['carrier'] = rdr.read().strip()
        with open(f'/sys/class/net/{iface}/address', encoding='utf-8') as rdr:
            sys[iface]['macaddr'] = rdr.read().strip()

    return sys


def make_plot_bitmap(rate):
    '''Convert bandwidth data to 4-character a dot-matrix plot'''
    bitmap = bytearray(32)
    # Drop any older data that we can't graph due to length
    rate = rate[-24:]
    max_q = max(rate)
    if max_q == 0:
        max_q = 1

    normalized_rates = [ int(i / max_q * 7) for i in reversed(rate) ]

    char_idx = 3
    while normalized_rates:
        bit = 0
        for peak in normalized_rates[:6]:
            bitmap[char_idx* 8 + 7 - peak] |= (1 << (bit))
            # Color in the area below the curve
            for extra in range (8 - peak, 8):
                bitmap[char_idx* 8 + extra] |= (1 << (bit))

            bit += 1

        char_idx -= 1
        normalized_rates = normalized_rates[6:]

    return bitmap


def bmon(lcd, interface='eth0'):
    '''Monitor the bandwith of the grandest, most bigliet interface'''
    bmon_format="$(element:name) $(attr:rxrate:bytes) $(attr:txrate:bytes)\n"

    try:
        with subprocess.Popen(["/usr/bin/bmon", "-p", interface,
                                "-o", f'format:fmt={bmon_format}'],
                                stdout=subprocess.PIPE) as proc:
            rxrate = []
            txrate = []

            while True:
                line = proc.stdout.readline().decode()
                _, rx_bps, tx_bps = line.split()
                rxrate.append(float(rx_bps))
                txrate.append(float(tx_bps))

                bitmap = bytearray()
                for datum in [rxrate, txrate]:
                    if len(datum) > 23:
                        datum.pop(0)
                    new_txrx = make_plot_bitmap(datum)
                    bitmap.extend(new_txrx)

                lcd.redefine_chars(0, bitmap)

    except FileNotFoundError as err:
        print(str(err))
        return

    except KeyboardInterrupt:
        print('Goodbye!')

def probe_display():
    '''Find a method to control the LCD display'''
    try:
        lcd = DotMatrixChardev()
    except FileNotFoundError:
        lcd =  DotMatrixI2C(1, 0x27)
    except PermissionError as err:
        # /dev/lcd exists, and we don't have proper permissions. We can't
        # fall back on DotMatrixI2C, so we must abort
        raise err

    return lcd

def netmain():
    '''From zie internyets!!!'''
    try:
        lcd = probe_display()
    except OSError as err:
        print(err)
        return

    sys = get_sys_info()

    lcd.main_screen_turn_on()
    lcd.write_line(0, (sys['hostname']).center(20))
    lcd.write_line(1, sys['eth0']['macaddr'].center(20))

    if sys['eth0']['carrier'] == "1":
        bw_monitor_interface = 'eth0'
        lcd.write_line(2, 'eth: ' + sys['eth0'].get('ip', ''))
    else:
        bw_monitor_interface = 'wlan0'
        if sys['wlan0']['carrier'] == "1":
            lcd.write_line(2, "wlan: " + sys['wlan0'].get('ip', ''))

    lcd.write_line(3, 'RX: \x00\x01\x02\x03    TX: \x04\x05\x06\x07')
    bmon(lcd, bw_monitor_interface)


if __name__ == "__main__":
    netmain()
