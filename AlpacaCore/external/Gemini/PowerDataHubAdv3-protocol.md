# Gemini Power & Data Hubs Advanced 3 serial protocol

Source: reverse-engineered for interoperability from the vendor's Windows ASCOM
driver, `ASCOM.GeminiPowerBoxPlusAdv3.Switch.dll` (driver version 2.6.0206,
installer "ASCOM.GeminiPowerAndDataHubAdv3.0.2 Setup.exe" from
https://geminiastro.cc/downloads). The vendor publishes no protocol document.
Confirmed on hardware 2026-09-08 (firmware 3.0.9): handshake, firmware reply, frame layout, and every set command below behaved exactly as described. See `AGENTS.md` for the hardware findings.

Product page: https://geminiastro.cc/products/powerbox-adv3/

## Link

| Item | Value |
|---|---|
| USB bridge | CH340/CH341 (USB vendor id `1a86`); the vendor ReadMe requires the CH341 driver |
| Serial | 19200 baud, 8N1, DTR and RTS asserted |
| Post-open settle | vendor sleeps 2000 ms after opening the port before the first command (CH340 DTR reset) |
| Command framing | ASCII `>X...#` followed by `\n` (vendor uses `SerialPort.WriteLine`) |
| Reply framing | ASCII `*X...#`, where `X` echoes the command letter; vendor reads with `ReadTo("#")` and strips the first two characters |
| Set commands | fire-and-forget; the vendor never reads a reply after `>O`, `>C`, `>X`, `>Y`, `>Z`, `>M` |

## Commands

| Command | Reply | Meaning |
|---|---|---|
| `>H#` | `*HGeminiPowerBoxPlusAdv3#` | identity; vendor compares the full string verbatim |
| `>V#` | `*V<nnn>#` | firmware as a 3-digit integer, e.g. `308` = 3.0.8; vendor refuses `< 308` |
| `>G#` | `*G<frame>#` | full status frame, see below |
| `>O<n>#` | none awaited | output `n` on, `n` = 1..11 |
| `>C<n>#` | none awaited | output `n` off |
| `>X<pct>#` | none awaited | DEW6 manual PWM, 0..100 |
| `>Y<pct>#` | none awaited | DEW7 manual PWM, 0..100 |
| `>Z10#` / `>Z11#` | none awaited | DEW6 off / on (Auto and Switch modes) |
| `>Z20#` / `>Z21#` | none awaited | DEW7 off / on |
| `>M10#` / `>M11#` / `>M12#` | none awaited | DEW6 mode Auto / Manual / Switch |
| `>M20#` / `>M21#` / `>M22#` | none awaited | DEW7 mode Auto / Manual / Switch |

Output channel numbers for `>O`/`>C`:

| n | Output |
|---|---|
| 1 | DC1 (always-on; the vendor driver never sends it) |
| 2..5 | DC2..DC5, 12 V switched |
| 6..11 | USB A..F (A and B are the USB 3.2 Gen1 ports) |

## Status frame (`>G#`)

The vendor strips `*G`, splits the remainder on the letters
`D U A T M B C S H V P` (C# `String.Split`, empty pieces kept) and indexes the
pieces positionally. The exact tag letters and their order are not known from
the decompile; only the positions are.

| Index | Content |
|---|---|
| 0 | 4 digits, DC2..DC5 on/off |
| 1 | 6 digits, USB A..F on/off |
| 2 | `1` if the AHT20 ambient sensor is attached |
| 3 | `1` if the DS18B20 lens probe is attached |
| 4 | DEW6 on/off flag used in Auto and Switch modes |
| 5 | DEW6 mode reported by the firmware; vendor re-sends `>M10#` when this is not `0` while it expects Auto |
| 6 | DEW7 on/off flag |
| 7 | DEW7 mode |
| 8 | DEW6 manual PWM percent |
| 9 | DEW7 manual PWM percent |
| 10 | DS18B20 lens temperature, degC |
| 11 | AHT20 ambient temperature, degC |
| 12 | AHT20 humidity, percent |
| 13 | dew point, degC |
| 14 | input voltage, V |
| 15 | DC12V output current, A |
| 16 | DC12V output power, W |

Confirmed on hardware: the tags are suffixes and the numbers are fixed-width
with leading spaces. A real frame:

```
*G1111D111111U1A1T1A1M1B1M100C100C 24.06S 23.39T 42.04H  9.76D12.6V 0.11C  1.38P#
```

The hub sends nothing unsolicited after power-up, but once it has answered a
`>G#` it keeps streaming `*G` frames on its own at about one every 3 s. That
is why the vendor's sensor window can read the buffer every 3 s without
sending anything. Set commands produce no reply.

## Vendor Switch surface (ISwitchV2, 18 switches)

| Id | Name | Writable | Range |
|---|---|---|---|
| 0..5 | USB A..F | yes | 0..1 |
| 6 | DC1 | no | always 1 |
| 7..10 | DC2..DC5 | yes | 0..1 |
| 11 | DEW6 | yes | 0..100 in Manual mode, 0..1 in Auto or Switch mode |
| 12 | DEW7 | yes | same |
| 13 | DEW6 Mode is | no | 0 Auto, 1 Manual, 2 Switch (set from the setup dialog, persisted) |
| 14 | DEW7 Mode is | no | same |
| 15 | Voltage(V) | no | 0..19, step 0.01 |
| 16 | Current(A) | no | 0..15, step 0.01 |
| 17 | Power(W) | no | 0..180, step 0.01 |

Auto mode requires both sensors. When either is missing the vendor driver
forces the channel back to Manual (`>M11#` or `>M21#`) and re-sends the manual
PWM value with `>X`/`>Y`. Temperature, humidity and dew point are only shown in
a pop-up window by the vendor; AlpacaBridge exposes them as read-only switches.
