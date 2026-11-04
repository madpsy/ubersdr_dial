# UberSDR Dial — ideas & roadmap

Running list of ideas as we go. Nothing here is committed to; tick things off or
strike them out as they land or get dropped.

## Operating modes

### 1. Standalone receiver (current focus)
The dial connects directly to an UberSDR instance over Wi-Fi and is the radio:
spectrum + waterfall on screen, knob tunes/zooms, audio out of the on-board
PCM5100A DAC + speaker.

### 2. Controller ("remote knob")
The dial drives a receiver session that is playing somewhere else — e.g. the
browser UI (v2 interface) or the UberSDR Audio desktop client. Open questions:
- **Desktop client:** `clients/ubersdr-audio` already exposes a local REST API
  (`/api/v1/tune`, `/api/v1/instances`, `/api/v1/connect`, `/api/v1/signal/stream`
  SSE …) — the dial could discover it via mDNS and drive it. Easiest path.
- **Browser v2 UI:** needs some way to pair the dial with a browser session.
  Options: server-side "linked controller" channel keyed by the user session
  UUID (pair via QR code shown in the browser → dial scans? no camera — so
  the browser shows a short pairing code you enter/confirm on the dial, or the
  dial shows a code you type in the browser); or a WebHID/WebSerial bridge when
  the dial is plugged in over USB (the original firmware was already a USB HID
  Surface Dial — we could keep a USB HID/serial control mode too).
- In controller mode the screen still shows freq/S-meter/spectrum mirrored
  from the controlled session.

### Bluetooth LE (ESP32-S3: BLE 5 only — no Classic, so no BT audio)
BLE is two-way but low bandwidth, and shares the 2.4 GHz radio/antenna with
Wi-Fi (time-sliced) plus ~30-60 KB internal RAM. Wi-Fi on this board proved
sensitive (HT40 stalls), so prefer BLE only when Wi-Fi isn't streaming:
- **BLE HID controller mode**: appear as a Surface Dial / media knob /
  keyboard to a PC, tablet or phone (natural fit for "controller" mode).
- **Improv Wi-Fi over BLE** for setup from a phone/Chrome page, no hotspot.
- **Web Bluetooth companion**: configure, push bookmarks, pick receivers.
- **BLE MIDI**: knob as a MIDI controller (v2 UI has WebMIDI controls).
- **Presence**: wake/dim the screen when your phone is near/away.

## Audio (phase 2)
- `/ws` audio WebSocket, Opus (`format=opus`, protocol v4 headers) — reference:
  `ka9q_ubersdr/clients/electron/monitor/minimal-radio.js`, `clients/ubersdr-audio`.
- Opus decode on the ESP32-S3 (libopus fits in 8 MB PSRAM), I²S to PCM5100A
  (BCK 3, WS 45, DO 42, MUTE 46).
- Volume on the knob (mode), mute by tap, squelch.
- S-meter from basebandPower/noisePower in the audio frames.

## UI / UX
- Modern dark round UI; tap centre chip to cycle knob function
  (tune / zoom / volume / filter / mode).
- Optional knob acceleration for fast spins — must show the effective step on
  screen while active (one click must always match the displayed STEP).
- Selectable tuning step (tap STEP to cycle) instead of only auto-from-span.
- Band quick-jump (arc of band buttons around the rim).
- Mode selector (USB/LSB/CW/AM/FM…) and filter width.
- Bookmarks (`/api/bookmarks` on the instance) and band plan overlay.
- WS2812 ring (13 LEDs) as tuning/S-meter/activity indicator.
- Screen dim / sleep after idle; wake on knob.

## Instances
- Public list from `https://instances.ubersdr.org/api/instances`
  (filters: `online_only`, `min_slots`, `lat/lon/max_dist`, band conditions).
- Favourites, sort by distance / free slots / SNR, show band predictions.
- Manual instance entry (local LAN receivers, password-protected ones).
- Local discovery via mDNS like `ubersdr-audio` does.

## Extras from the UberSDR APIs
- Decoder spots (FT8/WSPR/CW skimmer) overlaid on the spectrum.
- Space weather / band conditions screen.
- DX cluster spots — tap to tune.
- Notifications as toasts (like `ubersdr_cyd`).

## Platform
- OTA updates + web flasher (ESP Web Tools), as `ubersdr_cyd` does.
- Settings web page on the device (`ubersdr-dial.local`).
- Original vendor firmware backed up in `firmware_backup/` to restore.
