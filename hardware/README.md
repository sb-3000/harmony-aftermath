# Hardware

The hub is a XIAO ESP32-C3, an nRF24L01 radio and two IR LED drivers on a 24 × 18 hole
perfboard, with through-hole parts only.

![Schematic](schematic/ir_hub_full_schematic_ss8050.svg)

## Parts

- 1 × Seeed XIAO ESP32-C3
- 1 × nRF24L01 2.4 GHz radio module
- 1 × AMS1117 3.3 V regulator module (for the radio)
- 6 × TSAL6200 IR LEDs, 940 nm (hub)
- 2 × SS8050 NPN transistors
- 4 × 22 Ω resistors (LED strings and blaster)
- 2 × 220 Ω resistors (transistor bases)
- 2 × 10 kΩ resistors (base pull-downs)
- 1 × 470 µF and 1 × 10 µF electrolytic capacitors
- 2 × 100 nF ceramic capacitors
- 1 × jack for the Harmony IR mini blaster (match your blaster's plug)
- 1 × Logitech Harmony IR mini blaster (optional)
- 1 × perfboard, 24 × 18 holes
- 1 × Logitech Harmony Hub case (from a dead or retired hub)
- 1 × Logitech Harmony remote that works with the hub (tested with the Harmony Companion)

## Building notes

- The radio sits on top of the board.
- The six hub LEDs point two to the front, two up, one left and one right, copying the
  original Harmony Hub. Like the original, it can reach devices that aren't directly in
  front of it, by bouncing IR off walls and the ceiling.
- The Harmony mini blaster holds two LEDs in series.
- Keep D6, D8 and D9 free: D6 prints the boot log, and D8 and D9 decide how the chip starts.
