# Harmony Aftermath

A do-it-yourself replacement for the discontinued Logitech Harmony Hub. It runs on a
Seeed XIAO ESP32-C3 and keeps using the original Harmony remote, which talks to the hub
over a 2.4 GHz nRF24L01 radio.

It controls devices by:

- **IR**: NEC codes from the hub's own LEDs and a plug-in IR blaster
- **Bluetooth**: acts as a keyboard and media remote for a PC, including sleep
- **Wake-on-LAN**: wakes a sleeping PC over the network
- **HTTP**: switches a [Tasmota](https://tasmota.github.io/) relay, and reads its input to
  tell whether a device is on

Remote buttons start **activities** (for example "Watch PC": wake the PC, turn on the TV,
switch to HDMI 3), and each activity has its own button map. Everything is set up in one
JSON file that you upload over Wi-Fi. A web page and a small HTTP API work like the remote
from a browser or phone.

## Hardware

A Seeed XIAO ESP32-C3, an nRF24L01 radio, IR LEDs on two SS8050 transistors, and an
optional jack for a Harmony IR mini blaster. Tested with the Harmony Companion remote.
The schematic and building notes are in [`hardware/`](hardware/).

## Build and flash

This uses [PlatformIO](https://platformio.org/).

```bash
pio run -t upload
```

The first install has to go over USB, because it also writes the partition layout
(`partitions.csv`). After that, update over Wi-Fi from the hub's status page, or with:

```bash
curl -F firmware=@.pio/build/hub/firmware.bin http://harmony-aftermath.local/update
```

If new firmware can't reach Wi-Fi within 3 minutes, the hub rolls back to the previous one.

## First setup

1. **Wi-Fi.** With no saved network, the hub opens a **Harmony Aftermath** hotspot. Join it
   from a phone and the Wi-Fi page opens (or go to `http://192.168.4.1/wifi`). Pick your
   network and enter its password. The hotspot also opens if the hub can't connect for a
   minute, so you can fix a wrong password or a changed network.
2. **Config.** Copy `data/config.example.json` to `data/config.json`, edit it for your
   devices, and upload it:

   ```bash
   curl -X PUT --data-binary @data/config.json http://harmony-aftermath.local/config
   ```

   The hub checks the whole file and rejects it with a reason if anything is wrong.
3. **Remote.** On the **Devices** page, pair the Harmony remote. If you already know its
   address, put it in `remote.address` in the config instead.
4. **Bluetooth.** On the **Devices** page, press **Pair** next to the PC, then add the
   "Harmony Aftermath" keyboard from the PC's Bluetooth settings within a minute.

## Config

`data/config.example.json` is a complete working example.

There's no screen for setting up activities or mapping buttons. That's on purpose: an AI
assistant does it very well. Give it this README and your config, describe what you want
("long press TV starts Watch PC with the Denon receiver, and volume goes to the receiver"), and upload the
file it gives you. The hub checks every upload and rejects mistakes with a reason.

**`remote`**

- `address`: the remote's 10-digit radio address, or empty until it's paired.
- `channel`: the radio channel, normally 5.
- `buttons`: maps the remote's 6-digit button codes to names. Unknown buttons show up in
  the log with their code.
- `activity_buttons` and `activity_buttons_long`: which button starts which activity, on a
  short or long (0.8 s) press.

**`devices`**, each with a `transport`:

- `ir_nec`: `id` is the 4-hex-digit NEC address, and `commands` map names to 2-hex-digit
  codes.
- `bluetooth`: `commands` map names to keys: `KEY_UP`, `KEY_DOWN`, `KEY_LEFT`,
  `KEY_RIGHT`, `KEY_ENTER`, `KEY_ESC`, `KEY_BACKSPACE`, `KEY_TAB`, `KEY_SPACE`,
  `KEY_SHIFT`, `KEY_A` to `KEY_Z`, `MEDIA_PLAY_PAUSE`, `MEDIA_STOP`, `MEDIA_NEXT`,
  `MEDIA_PREVIOUS`, `MEDIA_MUTE`, `MEDIA_VOLUME_UP`, `MEDIA_VOLUME_DOWN` and
  `SYSTEM_SLEEP`.
- `tasmota`: `host` is the switch's address. Its commands are `on` and `off`.

Any device can also have:

- `wake_on_lan`: `mac` and `broadcast`, which enable the `wake_by_network` command.
- `state`: a Tasmota switch input that shows whether the device is on (`tasmota`,
  `switch`, `on_value`). This lets `"power": "on"` work for a device whose only power
  code is a toggle.

**`activities`**

- `name`: shown on the pages.
- `start` and `stop`: lists of steps. A step is `{"device", "command"}`,
  `{"device", "power": "on" | "off"}` or `{"delay_ms"}`. Starting an activity runs the
  previous activity's `stop` steps first.
- `buttons`: maps button names to a device command. Add `"repeat": true` to repeat while
  the button is held.
- `extends`: another activity whose buttons and steps this one inherits.

## Web pages and API

| Page | What it does |
|---|---|
| `/` | Status. Add `?format=json` for JSON |
| `/remote` | Activity and volume buttons for a phone |
| `/devices` | Pair the Harmony remote and Bluetooth devices |
| `/wifi` | Choose the Wi-Fi network |
| `/config` | `GET` shows the config, `PUT` replaces it |
| `/log` | Recent events |
| `/update` | `POST` new firmware |

| API | What it does |
|---|---|
| `GET /api/state` | Current activity, button names and activity list |
| `POST /api/button/{name}` | Presses a button, like the remote |
| `POST /api/activity/{id}` | Starts an activity |
| `POST /api/device/{name}/{command}` | Sends one device command |

There is no login. Anyone on your network can use the pages and API, including uploading
config and firmware.

## Tools

- `tools/read_serial.py` reads the hub's USB log without resetting it. On macOS, other
  serial monitors can reset the XIAO into download mode when they open the port.
- `tools/pin_test.py` checks the XIAO's pins from the ROM bootloader, for finding a
  damaged board or a bad solder joint.

## Limitations

- IR only sends NEC codes.
- If the radio is connected after the hub starts, it won't listen until a restart or a
  config upload.
- The setup hotspot is open, with no password.

## License

[MIT](LICENSE)
