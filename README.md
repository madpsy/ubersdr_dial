# UberSDR Dial

Firmware that turns the **Guition JC3636K718** into a hand-held front end for
[UberSDR](https://ubersdr.org). The JC3636K718 is a 1.8" round touch display
on a rotary knob, built around an ESP32-S3.

## Features

Each time it starts, the dial asks which mode to use. Your last choice is
already highlighted.

- **Receiver (Wi-Fi):** the dial is the radio. Pick a public UberSDR receiver
  from the list, and it streams audio to its own speaker while showing the
  spectrum and waterfall.
- **Controller (Bluetooth):** the dial is a remote knob for the UberSDR v2 web
  UI. Connect to it from the browser with Web Bluetooth. The dial then shows the
  page's frequency, signal and waterfall.

Both modes share the same screen:

- **Knob:** turns whatever page is selected: tune, volume, zoom, squelch,
  bandwidth or step size. Turning faster while tuning makes bigger steps.
- **Page buttons:** jump straight to a page.
- **Chip at the bottom:** tap for the next page, hold for the menu.
- **Waterfall:** drag to pan, tap to tune.
- **SNR ring:** shows signal strength around the edge. Drag the squelch notch
  along it to set squelch.
- **Frequency:** tap to open the band picker.
- **Mode button:** tap to step through modes, hold for the mode picker.
- **Volume button:** tap to mute.
- **Padlock:** locks tuning.
- **Header:** in Receiver mode, tap it to pick another receiver.
- **LED ring and haptics:** give feedback while you turn and tap.

Settings are kept across power cycles: Wi-Fi, the chosen receiver, the last
frequency, mode and view, and the menu options.

### First-time Wi-Fi setup

With no Wi-Fi configured, the dial starts a hotspot called
`UberSDR-Dial-XXXX`. Join it from a phone. A setup page opens; if it doesn't,
browse to `http://192.168.4.1/`. Choose your network and enter its password.
To change networks later, use **Wi-Fi setup** in the menu.

## Hardware

Guition JC3636K718:

- ESP32-S3R8, with 16 MB flash and 8 MB octal PSRAM
- 360×360 ST77916 display over QSPI, with CST816 touch
- Rotary encoder
- PCM5100A DAC and speaker
- WS2812 LED ring
- Haptic motor
- Battery

## Building

The firmware builds with [PlatformIO](https://platformio.org): the CLI or the
VS Code extension. The libraries are fetched automatically on the first build:
LVGL 9.2, NimBLE, WebSockets, ArduinoJson, ESP8266Audio (Opus) and
Adafruit NeoPixel.

```sh
pio run -e jc3636k718                 # build
pio run -e jc3636k718 -t upload       # build and flash over USB-C
```

The board uses the ESP32-S3's native USB, so no BOOT button is needed:
esptool resets it into download mode by itself. On Linux it appears as
`/dev/ttyACM0`.

To read the serial log, use `pio device monitor`. You can also open the port
yourself, but keep echo off. On Linux:

```sh
stty -F /dev/ttyACM0 -hupcl raw -echo 115200 && cat /dev/ttyACM0
```

`jc3636k718` is the environment to use. `jc3636k718-idf`, which builds Arduino
as an ESP-IDF component, is an experiment and isn't recommended.

### Layout

| Path | Contents |
| --- | --- |
| `src/hw/` | Board drivers: display and layered compositor, touch, encoder, LEDs, haptics, battery |
| `src/app/` | Receiver engine (`local_radio`), UberSDR spectrum and audio sockets (`sdr`, `audio`), Bluetooth link to v2 (`ble_ctrl`), settings, Wi-Fi setup |
| `src/ui/` | LVGL screens: the dial screen used by both modes (`screen_controller.cpp`), receiver picker, menus, fonts |
| `include/lv_conf.h` | LVGL configuration |
