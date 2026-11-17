#!/usr/bin/env python3
"""Reads the ESP32-C3's USB serial output without resetting the board.

macOS raises DTR and RTS when a port opens. On the C3's built-in USB, dropping
DTR while RTS is still high resets the chip, and DTR having been high makes it
restart in download mode. pyserial drops DTR first, so this drops RTS first.

Usage: read_serial.py [seconds] [port]
"""
import fcntl
import glob
import os
import select
import struct
import sys
import termios
import time

TIOCM_DTR = 0x002
TIOCM_RTS = 0x004
TIOCMBIC = 0x8004746B


def clear_line(fd, line):
    fcntl.ioctl(fd, TIOCMBIC, struct.pack("I", line))


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 15
    ports = sys.argv[2:] or glob.glob("/dev/cu.usbmodem*")
    if not ports:
        sys.exit("no board detected")

    fd = os.open(ports[0], os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    clear_line(fd, TIOCM_RTS)
    clear_line(fd, TIOCM_DTR)

    # Raw mode, and no HUPCL so closing the port doesn't toggle the lines either.
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = (attrs[2] & ~termios.HUPCL) | termios.CLOCAL | termios.CREAD
    attrs[3] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)

    end = time.time() + seconds
    while time.time() < end:
        ready, _, _ = select.select([fd], [], [], 0.2)
        if ready:
            data = os.read(fd, 4096)
            if data:
                sys.stdout.write(data.decode(errors="replace"))
                sys.stdout.flush()
    os.close(fd)


if __name__ == "__main__":
    main()
