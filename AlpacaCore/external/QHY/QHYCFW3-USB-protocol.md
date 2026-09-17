# QHYCFW3 USB serial protocol

Protocol summary for the standalone QHYCFW3 filter wheel driven over its own
USB cable (`qhy_cfw3_protocol_wrapper.cpp`). Sources: QHY's published
"Filter wheel serial protocol" command table (qhyccd.com, CFW3 product page,
image dated 2022-08-23), INDI `drivers/filter_wheel/qhycfw3.cpp`, the strings
in QHY's `arduloadeFW3.zip` firmware image, and a bench session against a
7-slot CFW3 (firmware `20181114`) on 2026-09-15. Items marked *bench* were
observed on that unit; everything else is per the vendor table.

## Transport

- USB socket is a Silicon Labs CP2102 UART bridge, USB `10c4:ea60`, udev
  by-id name `usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_0001-if00-port0`,
  enumerates as `/dev/ttyUSBn`. 9600 8N1, no flow control.
- The MCU is an ATmega328P (the firmware updater is an Arduino `avrdude`
  front end targeting "Arduino Duemilanove w/ ATmega328"). Opening the port
  asserts DTR, which pulses the MCU reset line. *Bench*: the wheel then homes
  (two full turns) and emits its position character about **17 s** after the
  open. Everything written before that byte is discarded. Clearing HUPCL
  before close did not avoid the reset on the next open.
- **Mode switch**: a push button inside the case selects USB control or 4-pin
  (camera) control. At power-on the LED flashes for one second: red = USB
  mode, green = 4-pin mode. In 4-pin mode the MCU still boots and homes on
  USB power and still emits the boot byte, but ignores every USB command.
  *Bench*: this looked exactly like a dead protocol until the switch was
  pressed with a paper clip.
- Power: QHY quotes 780 mA over USB during a move, above the USB 2.0 budget;
  a 12 V feed on the RJ11 socket drops the USB draw to under 100 mA. *Bench*:
  USB-only worked for a 7-slot wheel on a VM passthrough port.

## Commands

No terminators in either direction. Replies are bare ASCII.

| Command | Reply | Notes |
|---|---|---|
| `0`..`9`, `A`..`F` | the same character, on arrival | Goto slot 1..16 (0-based on the wire). Silent while turning, then the arrived slot. *Bench*: about 1 s per slot travelled, shortest direction. A goto to the current slot answers at once (firmware 201409+; older firmware stays silent). A slot at or beyond the count is ignored with no reply. |
| `VRS` | 8 chars `yyyymmdd` | Firmware date. *Bench*: `20181114`. |
| `MXP` | 1 char | **Slot count on this firmware** (*bench*: `7` on a 7-slot wheel; a goto to `7` was ignored). QHY's table documents it as the highest index (`4` = 5 positions, `F` = 16 positions). The driver treats the value as the count and validates goto against it; INDI does the same. If a wheel ever reports one less than its physical slots, this is the place to look. |
| `NOW` | 1 char | Current slot, 0-based. |
| `RESET` | position char when done | Re-home to slot 1 (per the firmware strings; not exercised by the driver). |

Firmware before 2014-09 supports only the goto characters (INDI: "firmware
must be higher than 201409"); the driver refuses to connect to such a wheel
because it cannot learn the slot count.

## Driver behaviour built on this

- Connect: open, wait up to 25 s for the boot byte (proceeds at once when it
  arrives, or without it when the port did not reset), then `VRS` (optional),
  `MXP` and `NOW` (required). The first connect therefore takes about 18 s on
  hardware; the fd is held across disconnects so later connects skip the open
  and the boot (ConformU abandons a Platform 7 `Connect()` after 5 s).
- Position is never polled on the wire: the settled slot is cached from the
  handshake and from each goto's arrival reply, and reads `-1` while a goto
  is in flight. The arrival wait runs on a joinable worker thread; disconnect
  cancels and joins it and the wheel finishes the move on its own.
- A same-slot goto is skipped in the driver, so pre-201409 silence there
  cannot stall a move.
- Auto-detect probes every CP210x serial bridge with the same boot-wait
  handshake. Each probe resets the device behind the port; an explicit port
  is recommended when other CP210x adapters are attached.
