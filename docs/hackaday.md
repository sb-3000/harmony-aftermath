# Hackaday.io project text

Paste each section into the matching field on hackaday.io.
The hackaday.io editor doesn't use Markdown, so set headings and lists with its toolbar.

---

## Project name

Harmony Aftermath: a new brain for the Logitech Harmony Hub

## Summary

An ESP32-C3 replacement brain for the discontinued Harmony Hub. It keeps the original remote, fills the room with IR, and adds Wake-on-LAN, Tasmota and Bluetooth.

## Description

I love my Logitech Harmony remote. The buttons are in the right places, it fits the hand,
and it controls the whole living room without anyone pointing it at anything.
Then Logitech stopped making Harmony. The device database isn't updated anymore, so I couldn't add my newer gear. Setting up a brand-new hub needed
a Windows PC. And I couldn't extend the hub at all: I wanted it to wake my PC over the
network, switch a Tasmota relay over HTTP, and run some logic of my own.

So I'm building a new brain for the hub. Harmony Aftermath is an ESP32-C3 board that:

- talks to the original Harmony remote over its 2.4 GHz radio, just like the Logitech hub
- fills the room with IR from six LEDs pointing in different directions, the same way the
  original hub reaches devices that aren't in front of it
- drives a plug-in Harmony IR mini blaster for gear hidden in a cabinet
- pairs with a Harmony remote from a web page, no Logitech account or Windows PC needed
- adds what the original never had: Wake-on-LAN, Tasmota switches over HTTP, and a
  Bluetooth keyboard and media remote for a PC, including sleep
- is meant to fit inside the original Harmony Hub case

Everything is set up in one JSON file uploaded over Wi-Fi. The firmware is open source
(MIT): https://github.com/sb-3000/harmony-aftermath

## Details

### Why

The Harmony Hub is a great idea that's slowly being abandoned. My problems with it:

- **New devices.** The device database is frozen, so newer gear isn't in it.
- **Setup.** Setting up a new hub needed the desktop software on a Windows PC.
- **No extensions.** I wanted the TV button to wake my PC over the network, turn on the
  TV, switch to the right HDMI input and turn on the speakers through a Tasmota relay.
  The original hub can't do half of that.

The remote, though, is still perfect. So the plan: keep the remote, replace the brain.

### How the remote talks to the hub

The Harmony remote doesn't use IR to talk to the hub. It uses a 2.4 GHz radio that an
nRF24L01, which anyone can buy for a dollar, can listen to. The protocol is simple once
you see it: an address, a channel, and packets with 6-digit button codes, plus "held"
and "released" messages.

The hub listens with an nRF24L01 on the same channel. Pairing works from the hub's Devices
page: press the button, and the hub listens for the remote's address and confirms it a
couple of times before saving it. A tip if you're decoding it yourself: my remote's
address only worked byte-reversed.

### IR that fills the room

The original hub has IR LEDs pointing in several directions and bounces light off the
walls and ceiling, so the TV doesn't need to be in front of it. I copied that: six IR LEDs,
two to the front, two up, one left and one right, in three strings of two, switched by an
SS8050 transistor.

A second SS8050 drives a jack for the Harmony IR mini blaster (two LEDs in series),
for gear inside a cabinet.

The ESP32-C3's RMT peripheral generates the NEC timing and the 38 kHz carrier in hardware,
so the timing stays clean even while Wi-Fi and Bluetooth are busy.

### Beyond what the original could do

Each device in the config has a transport:

- **IR (NEC)**: TVs, DACs, amplifiers
- **Bluetooth**: the hub appears to the PC as a keyboard and media remote, including a
  sleep key
- **Wake-on-LAN**: wakes the PC from sleep
- **HTTP to Tasmota**: switches a relay on and off, and reads a Tasmota input to know
  whether a device is really on. That makes "power on" work for gear that only has a
  toggle code.

Remote buttons start **activities**. My "Watch PC" activity wakes the PC over the network,
turns on the TV, switches it to HDMI 3, and maps volume to the TV. A long press on the
same button starts a version that turns on my RME DAC and sends volume there instead.
Starting an activity first runs the previous one's stop steps, like the original hub does.

There's also a phone-friendly web remote, and a small HTTP API, so home automation can
press buttons or start activities too.

### Setup without a Windows PC

- **Wi-Fi.** With no saved network, the hub opens a "Harmony Aftermath" hotspot with a
  captive portal. Pick your network from a phone.
- **Config.** One JSON file with the remote's buttons, devices and activities, uploaded
  with a single curl command. The hub checks the whole file and rejects it with a reason if
  anything is wrong.
- **Firmware updates** go over Wi-Fi. If new firmware can't reach Wi-Fi within three
  minutes, the hub rolls back to the previous version by itself.

There's no setup screen for activities, on purpose. An AI assistant is very good at
editing this kind of JSON: describe what you want ("long press TV starts Watch PC with the
DAC, and volume goes to the DAC"), and upload what it gives you. The hub's validation
catches mistakes.

### Hardware

- Seeed XIAO ESP32-C3
- nRF24L01 radio
- 6 IR LEDs for the hub, plus a jack for the Harmony IR mini blaster
- 2 × SS8050 NPN transistors as LED switches
- Through-hole parts only, on a 24 × 18 hole perfboard sized for the original hub case

### What went wrong along the way

- **The first IR attempt was weak.** I started with two cheap YS-IRTM serial IR modules.
  They worked, but only at 1–3 m and only when aimed. Direct-driven LEDs on transistors
  fixed it.
- **The MOSFET that wasn't.** I wanted an IRLB8721 as the LED switch, but it barely opened
  at 3.3 V on the gate. The SS8050 works fine from a 3.3 V pin.
- **"Dead" hardware that wasn't dead.** I lost hours to a board that looked broken. It was
  the USB cable. Then the radio never heard the remote, and that was a missing antenna.
  Now I try a known-good cable and check the antenna before blaming anything else.
- **"IR can't wake the TV."** It could. The LEDs had just lost their aim. Now I send Mute
  first to prove the IR link before I trust any IR failure.
- **My AI coding partner said no.** I wrote much of this firmware with Claude, and at first
  it didn't want to write code to put the nRF24L01 into promiscuous mode. It saw sniffing
  radio traffic as something it shouldn't help with, even though the only thing I wanted
  to listen to was my own remote. It came around in the end, and pairing now uses the
  classic trick: an illegal 2-byte address width, no CRC and no auto-ack, listening for
  the 00 AA / 00 55 preamble to find the remote's address.
- **The ESP32-C3's boot pins.** D8 and D9 decide how the chip starts, and D6 prints the boot
  log, so I keep all three free.

### Status

Working in my living room with the Harmony Companion remote: IR to an LG OLED TV
and an RME DAC, Wake-on-LAN to the PC, a Tasmota speaker switch, and Bluetooth to the PC.

Next:

- fit the board into the original Harmony Hub case
- test more Harmony remote models
- more IR protocols than NEC

---

## Components

- 1 × Seeed XIAO ESP32-C3
- 1 × nRF24L01 2.4 GHz radio module
- 1 × AMS1117 3.3 V regulator module (for the radio)
- 6 × IR LEDs, 940 nm (hub)
- 2 × SS8050 NPN transistors
- 4 × 22 Ω resistors (LED strings and blaster)
- 2 × 220 Ω resistors (transistor bases)
- 2 × 10 kΩ resistors (base pull-downs)
- 1 × 470 µF + 1 × 10 µF electrolytic capacitors
- 2 × 100 nF ceramic capacitors
- 1 × jack for the Harmony IR mini blaster (match your blaster's plug)
- 1 × Logitech Harmony IR mini blaster (optional)
- 1 × perfboard, 24 × 18 holes
- 1 × Logitech Harmony Hub case (from a dead or retired hub)
- 1 × Logitech Harmony remote that works with the hub (tested: Harmony Companion)
