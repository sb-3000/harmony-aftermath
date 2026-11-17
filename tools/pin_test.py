#!/usr/bin/env python3
"""Tests the XIAO ESP32-C3's pins through the ROM bootloader.

The chip must already be in the bootloader, and nothing should be attached to
the pins. Boot pins GPIO 8 and 9 are only read, never driven.
"""
import fcntl
import glob
import os
import select
import struct
import termios
import time

GPIO_OUT_W1TS = 0x60004008
GPIO_OUT_W1TC = 0x6000400C
GPIO_ENABLE_W1TS = 0x60004024
GPIO_ENABLE_W1TC = 0x60004028
GPIO_IN = 0x6000403C
OUT_SEL_BASE = 0x60004554
IO_MUX_BASE = 0x60009004
IE, WPU, WPD = 1 << 9, 1 << 8, 1 << 7
MCU_SEL_GPIO = 1 << 12

TIOCM_DTR = 0x002
TIOCM_RTS = 0x004
TIOCMBIC = 0x8004746B

LABELS = {2: "D0", 3: "D1 (CE)", 4: "D2 (SCK)", 5: "D3 (MISO)", 6: "D4 (MOSI)", 7: "D5 (CSN)", 21: "D6", 20: "D7", 10: "D10"}
BOOT_PINS = {8: "D8", 9: "D9"}


def open_port():
    fd = os.open(glob.glob("/dev/cu.usbmodem*")[0], os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    # RTS first: see read_serial.py.
    fcntl.ioctl(fd, TIOCMBIC, struct.pack("I", TIOCM_RTS))
    fcntl.ioctl(fd, TIOCMBIC, struct.pack("I", TIOCM_DTR))
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = (attrs[2] & ~termios.HUPCL) | termios.CLOCAL | termios.CREAD
    attrs[3] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


fd = open_port()


def send(op, data):
    pkt = bytes([0x00, op]) + struct.pack("<H", len(data)) + b"\0\0\0\0" + data
    pkt = pkt.replace(b"\xdb", b"\xdb\xdd").replace(b"\xc0", b"\xdb\xdc")
    os.write(fd, b"\xc0" + pkt + b"\xc0")


def drain(seconds=0.05):
    got = b""
    end = time.time() + seconds
    while time.time() < end:
        ready, _, _ = select.select([fd], [], [], 0.01)
        if ready:
            got += os.read(fd, 4096)
    return got


def rd(addr):
    for _ in range(3):
        send(0x0a, struct.pack("<I", addr))
        reply = drain(0.08)
        i = reply.find(b"\xc0\x01\x0a")
        if i >= 0:
            return struct.unpack("<I", reply[i + 5:i + 9])[0]
    raise RuntimeError(f"no reply reading {addr:#x}")


def wr(addr, value):
    send(0x09, struct.pack("<IIII", addr, value, 0xFFFFFFFF, 0))
    drain(0.05)


def level(pin):
    time.sleep(0.01)
    return (rd(GPIO_IN) >> pin) & 1


def sync():
    got = b""
    for _ in range(4):
        send(0x08, bytes([0x07, 0x07, 0x12, 0x20]) + b"\x55" * 32)
        got += drain(0.3)
    if b"\xc0\x01\x08" not in got:
        raise SystemExit("bootloader did not answer")
    drain(0.3)


def main():
    sync()
    print(f"{'pin':<16}{'pull-up':<9}{'pull-down':<11}{'drive high':<12}{'drive low':<11}result")
    for pin, label in LABELS.items():
        mux = IO_MUX_BASE + 4 * pin
        wr(GPIO_ENABLE_W1TC, 1 << pin)
        wr(mux, MCU_SEL_GPIO | IE | WPU)
        pull_up = level(pin)
        wr(mux, MCU_SEL_GPIO | IE | WPD)
        pull_down = level(pin)
        wr(mux, MCU_SEL_GPIO | IE)
        wr(OUT_SEL_BASE + 4 * pin, 0x80)
        wr(GPIO_OUT_W1TS, 1 << pin)
        wr(GPIO_ENABLE_W1TS, 1 << pin)
        high = level(pin)
        wr(GPIO_OUT_W1TC, 1 << pin)
        low = level(pin)
        wr(GPIO_ENABLE_W1TC, 1 << pin)
        ok = (pull_up, pull_down, high, low) == (1, 0, 1, 0)
        print(f"GPIO{pin:<2} {label:<10}{pull_up:<9}{pull_down:<11}{high:<12}{low:<11}{'ok' if ok else 'PROBLEM'}")

    print("\nBoot pins, input only (no output driven):")
    for pin, label in BOOT_PINS.items():
        mux = IO_MUX_BASE + 4 * pin
        original = rd(mux)
        wr(mux, (original & ~(WPU | WPD)) | IE)
        no_pull = level(pin)
        wr(mux, (original & ~WPD) | IE | WPU)
        pull_up = level(pin)
        wr(mux, original)
        print(f"GPIO{pin} {label}: no pull={no_pull}, internal pull-up={pull_up}")
    os.close(fd)


if __name__ == "__main__":
    main()
