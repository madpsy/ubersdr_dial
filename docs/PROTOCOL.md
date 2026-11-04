# Controller protocol

In Controller mode the dial is a remote for the UberSDR v2 web UI (SDR Control
→ Dial). The page does the receiving and sends the dial what the receiver is
doing. The dial sends back knob turns and taps.

There are two ways to connect, chosen on the dial's start screen:

- **Bluetooth:** Web Bluetooth, with one GATT characteristic per record.
- **USB:** Web Serial over the dial's USB-C port, with the records framed.

The records are the same either way. The page's side is
`static/v2/src/controls/dial.js` in ka9q_ubersdr; change both together. All
multi-byte fields are little-endian.

## Records

### Input (dial → page)

Input is a run of 2-byte records, `[code u8][value i8]`. Several records may
travel together.

| Code | Meaning |
| --- | --- |
| 1–6 | Knob turned on page tune / volume / zoom / squelch / bandwidth / step. Value = signed detents, + for clockwise. |
| 16–22 | Mode tap / hold, frequency tap / hold, mute tap / hold, centre tap |
| 32 + i | Set mode `i` (v2's mode table order) |
| 48 + i | Go to band `i` (160 m … 6 m) |
| 64 + i | Set tuning step `i` (v2's `TUNING_STEPS`) |
| 80–85 | Lock toggle, squelch on/off, auto squelch, zoom in, zoom out, zoom reset |
| 96 | Waterfall lines per second wanted (1–20) |
| 97 | Waterfall drag. Value = pixels moved out of 360, + for right. |
| 98 | Tap on the waterfall. Value = −127…127 across the view; tune there. |

### State (page → dial), 20 bytes

| Byte | Field |
| --- | --- |
| 0 | Version = 1 |
| 1–4 | Frequency in Hz, u32 |
| 5 | Mode index, 255 = unknown |
| 6 | SNR in dB, i8; −128 = no reading |
| 7 | Flags: b0 audio up, b1 squelch on, b2 squelch open, b3 muted, b4 tuning locked |
| 8 | Volume, 0–100 |
| 9 | Squelch threshold in dB, i8; −128 = off |
| 10–11 | Passband low edge in Hz, i16 |
| 12–13 | Passband high edge in Hz, i16 |
| 14–17 | Tuning step in Hz, u32 |
| 18–19 | Reserved |

### Name (page → dial)

The receiver's name, UTF-8, at most 32 bytes.

### Spectrum line (page → dial)

| Byte | Field |
| --- | --- |
| 0 | Version = 1 |
| 1 | Line sequence number, u8 |
| 2 | Index of the first point |
| 3 | Point count |
| 4–7 | Centre frequency in Hz, u32 |
| 8–11 | Span in Hz, u32 |
| 12… | One byte per point; dB = byte / 2 − 160 |

A line has 160 points. Over Bluetooth it is sent whole, or in 8-point pieces
where the link won't take a long write. Over USB it is always sent whole.

## Bluetooth

The service is `89540001-8c77-4d5f-9411-e7534b50c873`, and the dial advertises
it. Its name is `UberSDR Dial XXXX`.

| Characteristic | UUID | Property | Record |
| --- | --- | --- | --- |
| input | `89540002-…` | notify | Input |
| state | `89540003-…` | write | State |
| name | `89540004-…` | write | Name |
| spec | `89540005-…` | write | Spectrum line |

The dial asks for a 7.5–15 ms connection interval and a 247-byte MTU.

## USB

The dial's USB port is the ESP32-S3's built-in USB-Serial/JTAG (VID `0x303a`,
PID `0x1001`). The baud rate doesn't matter. Opening the port must not toggle
DTR or RTS, because that resets the chip into its bootloader.

The same port also carries the firmware's log text, so every record travels in
a frame:

```
0x00  COBS( [type u8][payload …][crc16 u16 LE] )  0x00
```

- **CRC:** CRC-16/CCITT-FALSE (polynomial 0x1021, initial value 0xFFFF),
  computed over the type and payload.
- **Splitting:** COBS leaves no zero bytes inside a frame, and log text never
  contains one. A reader splits on zero bytes and drops any chunk that doesn't
  decode to a frame with a good CRC. That chunk was log text.

| Type | Direction | Payload |
| --- | --- | --- |
| 1 HELLO | page → dial | `[1]`. Sent when the port opens, then once a second. |
| 1 HELLO | dial → page | `[1][name, UTF-8]`. The answer to each HELLO. |
| 2 INPUT | dial → page | Input records |
| 3 STATE | page → dial | State record |
| 4 NAME | page → dial | Name |
| 5 SPEC | page → dial | Spectrum line |
| 6 BYE | page → dial | None. The page is closing the port. |

A HELLO connects. After that, any frame keeps the link up. The dial goes back
to waiting after 3 s without a frame, and the page after 4 s. The page leaves
the port open and keeps saying HELLO, so it picks the dial up again when the
dial restarts into Controller · USB.
