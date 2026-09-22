#!/usr/bin/env python3
"""Capture a Sky-Watcher motor-controller board's identity/geometry from real
hardware and emit a `FakeMountProfile` C++ block for
`AlpacaCore/tests/fake_skywatcher_mount.h`.

This is a READ-ONLY bench tool: it only ever sends the protocol's inquiry
commands (":e", ":a", ":b", ":g", ":q", ":s"), never a motion command. Safe to
run against a bare, powered mount at any time.

Protocol details (frame shape, byte layout, u24 nibble order) are taken
directly from AlpacaCore/src/vendors/skywatcher/skywatcher_protocol_wrapper.cpp
so the numbers this script reports are exactly what the driver itself would
compute from the same wire bytes -- not a reimplementation guess. See
.github/instructions/skywatcher.instructions.md for the protocol and board
background (EQ-class Synta boards section) before using this on a new board.

Two transports -- use whichever access you actually have to the board:

  Serial (direct access to the host with the port, board must be otherwise
  idle -- AlpacaBridge itself must NOT be holding the port open):
    pip install pyserial
    python3 capture_skywatcher_profile.py --port /dev/ttyUSB0 --name eqm35_pro

  Alpaca (a running AlpacaBridge instance exposing the board as a Telescope
  device -- goes through CommandString/Raw, the same passthrough
  command_string() gives any Alpaca client; connects the device first if it
  is not already connected, and leaves it exactly as it found it):
    python3 capture_skywatcher_profile.py \\
        --alpaca-url https://your-host/api/v1/telescope/0 --name eqm35_pro

Baud (serial only): tried in order 115200 (mount's own built-in USB port on
Synta EQ boards, e.g. the EQM-35 Pro's soldered PL2303) then 9600 (EQDIR
cable / STM32 CDC-ACM, where the rate is actually ignored). Override with
--baud to skip the probe.
"""

from __future__ import annotations

import argparse
import datetime
import sys
import urllib.request
import urllib.parse
import json

kAxisRa = 1
kAxisDec = 2

# Mirrors mount_code_to_name() in skywatcher_protocol_wrapper.cpp. Keep in
# sync by hand -- there is no shared source of truth to generate this from.
MOUNT_CODE_TO_NAME = {
    0x00: "EQ6",
    0x01: "HEQ5",
    0x02: "EQ5",
    0x03: "EQ3",
    0x04: "EQ8",
    0x05: "AZ-EQ6",
    0x06: "AZ-EQ5",
    0x0A: "Star Adventurer",
    0x0C: "Star Adventurer GTi",
    0x20: "EQ8-R Pro",
    0x22: "AZ-EQ6 Pro",
    0x23: "EQ6-R Pro",
    0x24: "EQ6 Pro",
    0x25: "CQ350 Pro",
    0x31: "EQ5 Pro",
    0x32: "EQM-35 Pro",
    0x44: "Wave 100i",
    0x45: "Wave 150i",
    0xA2: "AZ-GTe",
    0xA5: "AZ-GTi",
}

# Feature word bits, from the EQ-class boards section of
# .github/instructions/skywatcher.instructions.md and EQMod's own set.
FEATURE_BITS = {
    0x0004: "HOME_INDEXER",
    0x0008: "IS_AZEQ",
    0x1000: "POLAR_LED",
    0x2000: "COMMON_SLEW_START",
    0x4000: "HALF_CURRENT_TRACKING",
}


class MotorControllerError(Exception):
    pass


def mount_code_to_name(code: int) -> str:
    return MOUNT_CODE_TO_NAME.get(code, f"Mount (code 0x{code:02X})")


def encode_u24(value: int) -> str:
    """0x123456 -> "563412": low byte first, each byte high-nibble-first."""
    b0 = value & 0xFF
    b1 = (value >> 8) & 0xFF
    b2 = (value >> 16) & 0xFF
    return f"{b0:02X}{b1:02X}{b2:02X}"


def decode_u24(data: str) -> int:
    """"563412" -> bytes 0x56, 0x34, 0x12 -> 0x123456."""
    if len(data) < 6:
        raise MotorControllerError(f"reply too short for 24-bit value: '{data}'")
    b0 = int(data[0:2], 16)
    b1 = int(data[2:4], 16)
    b2 = int(data[4:6], 16)
    return b0 | (b1 << 8) | (b2 << 16)


def decode_mc_version(data: str):
    """":e" reply layout is <fw major><fw minor><mount code>, NOT reversed --
    verified against decode_mc_version() in skywatcher_protocol_wrapper.cpp."""
    if len(data) < 6:
        raise MotorControllerError(f"short version reply: '{data}'")
    fw_major = int(data[0:2], 16)
    fw_minor = int(data[2:4], 16)
    mount_code = int(data[4:6], 16)
    return fw_major, fw_minor, mount_code


def format_fw_version(major: int, minor: int) -> str:
    return f"{major}.{minor:02d}"


class SerialLink:
    """Talks the MC protocol directly over a serial port."""

    def __init__(self, ser, verbose: bool = False):
        self.ser = ser
        self.verbose = verbose

    def send_command(self, command: str, axis: int, data: str = "", retries: int = 3) -> str:
        if axis not in (kAxisRa, kAxisDec):
            raise ValueError(f"invalid axis {axis}")
        frame = f":{command}{axis}{data}\r"
        last_err = None
        for attempt in range(retries):
            self.ser.reset_input_buffer()
            self.ser.write(frame.encode("ascii"))
            self.ser.flush()
            reply = self._read_reply()
            if self.verbose:
                print(f"  MC {frame[:-1]} -> {reply!r}", file=sys.stderr)
            if reply is None:
                last_err = f"no reply to '{frame[:-1]}' (attempt {attempt + 1}/{retries})"
                continue
            if reply.startswith("="):
                return reply[1:]
            if reply.startswith("!"):
                raise MotorControllerError(
                    f"board rejected '{frame[:-1]}': error code {reply[1:]!r}"
                )
            last_err = f"malformed reply to '{frame[:-1]}': {reply!r}"
        raise MotorControllerError(last_err or "unknown error")

    def _read_reply(self):
        buf = bytearray()
        while True:
            byte = self.ser.read(1)
            if not byte:
                return None if not buf else buf.decode("ascii", errors="replace")
            if byte == b"\r":
                return buf.decode("ascii", errors="replace")
            buf += byte


def probe_baud(port: str, timeout_s: float, verbose: bool):
    """Try 115200 then 9600, per the EQ-class boards section of the
    skywatcher instructions (built-in USB ports answer only at 115200; EQDIR
    cables and the Wave's STM32 CDC-ACM port answer at 9600 / ignore baud)."""
    try:
        import serial
    except ImportError:
        print("error: pyserial is required for --port (`pip install pyserial`)", file=sys.stderr)
        sys.exit(1)

    for baud in (115200, 9600):
        print(f"Probing {port} at {baud} baud...", file=sys.stderr)
        try:
            ser = serial.Serial(
                port,
                baudrate=baud,
                bytesize=8,
                parity="N",
                stopbits=1,
                timeout=timeout_s,
            )
        except serial.SerialException as exc:
            raise MotorControllerError(f"cannot open {port}: {exc}") from exc
        link = SerialLink(ser, verbose=verbose)
        try:
            link.send_command("e", kAxisRa, retries=1)
            print(f"  -> answered at {baud} baud", file=sys.stderr)
            return link, baud
        except MotorControllerError:
            ser.close()
            continue
    raise MotorControllerError(
        f"no reply from {port} at 115200 or 9600 baud -- check cabling/power"
    )


class AlpacaLink:
    """Talks the MC protocol through a live AlpacaBridge instance's Telescope
    CommandString/Raw passthrough (command_string() -> send_raw_command()),
    the same one any Alpaca client can call -- no serial/SSH access needed.

    Connects the device if it is not already connected (harmless: the normal
    connect sequence is :e/:a/:b/:g reads, plus :F/:E ONLY if the board
    reports not-initialized -- the same thing any client does on connect) and
    restores the PRIOR connected state on close(), never leaving a device
    connected that this script itself connected... nor disconnecting one a
    real client already had open.
    """

    def __init__(self, base_url: str, client_id: int = 1, timeout_s: float = 10.0, verbose: bool = False):
        self.base_url = base_url.rstrip("/")
        self.client_id = client_id
        self.timeout_s = timeout_s
        self.verbose = verbose
        self._tx_id = 0
        self._we_connected = False

    def _request(self, method: str, path: str, params: dict) -> dict:
        self._tx_id += 1
        params = {**params, "ClientID": self.client_id, "ClientTransactionID": self._tx_id}
        url = f"{self.base_url}/{path}"
        data = urllib.parse.urlencode(params).encode("ascii")
        if method == "GET":
            req = urllib.request.Request(f"{url}?{data.decode()}", method="GET")
        else:
            req = urllib.request.Request(url, data=data, method="PUT")
        with urllib.request.urlopen(req, timeout=self.timeout_s) as resp:
            body = json.loads(resp.read().decode("utf-8"))
        if self.verbose:
            print(f"  {method} {path} {params} -> {body}", file=sys.stderr)
        if body.get("ErrorNumber", 0) != 0:
            raise MotorControllerError(
                f"Alpaca error on {path}: {body.get('ErrorNumber')} {body.get('ErrorMessage')}"
            )
        return body

    def get_connected(self) -> bool:
        return bool(self._request("GET", "connected", {}).get("Value"))

    def set_connected(self, value: bool):
        self._request("PUT", "connected", {"Connected": "true" if value else "false"})

    def ensure_connected(self):
        if self.get_connected():
            return
        print("  device not connected -- connecting (normal connect sequence)...", file=sys.stderr)
        self.set_connected(True)
        self._we_connected = True

    def restore(self):
        if self._we_connected:
            print("  restoring device to its original disconnected state...", file=sys.stderr)
            self.set_connected(False)
            self._we_connected = False

    def send_command(self, command: str, axis: int, data: str = "", retries: int = 3) -> str:
        if axis not in (kAxisRa, kAxisDec):
            raise ValueError(f"invalid axis {axis}")
        frame = f":{command}{axis}{data}"
        last_err = None
        for attempt in range(retries):
            try:
                body = self._request("PUT", "commandstring", {"Command": frame, "Raw": "true"})
            except MotorControllerError as exc:
                last_err = str(exc)
                continue
            reply = body.get("Value", "")
            if reply.startswith("="):
                return reply[1:]
            if reply.startswith("!"):
                raise MotorControllerError(f"board rejected '{frame}': error code {reply[1:]!r}")
            last_err = f"malformed reply to '{frame}': {reply!r} (attempt {attempt + 1}/{retries})"
        raise MotorControllerError(last_err or "unknown error")


def capture(link) -> dict:
    result = {}

    e1 = link.send_command("e", kAxisRa)
    fw_major, fw_minor, mount_code = decode_mc_version(e1)
    result["version_reply"] = e1
    result["fw_major"] = fw_major
    result["fw_minor"] = fw_minor
    result["fw_string"] = format_fw_version(fw_major, fw_minor)
    result["mount_code"] = mount_code
    result["model_name"] = mount_code_to_name(mount_code)

    cpr = {}
    timer_freq = {}
    for axis, label in ((kAxisRa, "ra"), (kAxisDec, "dec")):
        cpr[label] = decode_u24(link.send_command("a", axis))
        timer_freq[label] = decode_u24(link.send_command("b", axis))
    result["cpr"] = cpr
    result["timer_freq"] = timer_freq

    g1 = link.send_command("g", kAxisRa)
    result["high_speed_ratio_reply"] = g1
    result["high_speed_ratio"] = decode_u24(g1 + "0000") & 0xFF

    features = {}
    for axis, label in ((kAxisRa, "ra"), (kAxisDec, "dec")):
        features[label] = decode_u24(link.send_command("q", axis, encode_u24(0x000001)))
    result["features"] = features

    steps_per_worm = {}
    for axis, label in ((kAxisRa, "ra"), (kAxisDec, "dec")):
        try:
            steps_per_worm[label] = decode_u24(link.send_command("s", axis, retries=1))
        except MotorControllerError as exc:
            steps_per_worm[label] = None
            print(f"  note: ':s{axis}' ({label}) did not answer: {exc}", file=sys.stderr)
    result["steps_per_worm"] = steps_per_worm

    return result


def decode_feature_bits(value: int) -> str:
    names = [name for bit, name in FEATURE_BITS.items() if value & bit]
    unknown = value & ~sum(FEATURE_BITS.keys())
    if unknown:
        names.append(f"unknown(0x{unknown:04X})")
    return " | ".join(names) if names else "(none)"


def print_summary(cap: dict, port: str, baud):
    print()
    print("=== Capture summary ===")
    print(f"Port/baud:        {port} @ {baud}")
    print(f"':e1' reply:      {cap['version_reply']!r}")
    print(f"Firmware:         {cap['fw_string']}")
    print(f"Mount code:       0x{cap['mount_code']:02X}")
    print(f"Model:            {cap['model_name']}")
    if cap["mount_code"] not in MOUNT_CODE_TO_NAME:
        print(
            "                  ^ NOT in MOUNT_CODE_TO_NAME / mount_code_to_name() -- "
            "a new board, add it there too"
        )
    ra_cpr, dec_cpr = cap["cpr"]["ra"], cap["cpr"]["dec"]
    print(f"CPR (RA / Dec):   {ra_cpr} / {dec_cpr}" + ("  <-- MISMATCH" if ra_cpr != dec_cpr else ""))
    ra_tf, dec_tf = cap["timer_freq"]["ra"], cap["timer_freq"]["dec"]
    print(f"Timer Hz (RA/Dec):{ra_tf} / {dec_tf}" + ("  <-- MISMATCH" if ra_tf != dec_tf else ""))
    print(f"High-speed ratio: {cap['high_speed_ratio']} (raw ':g1' = {cap['high_speed_ratio_reply']!r})")
    ra_feat, dec_feat = cap["features"]["ra"], cap["features"]["dec"]
    mismatch = "  <-- MISMATCH" if ra_feat != dec_feat else ""
    print(f"Features (RA):    0x{ra_feat:04X}  [{decode_feature_bits(ra_feat)}]")
    print(f"Features (Dec):   0x{dec_feat:04X}  [{decode_feature_bits(dec_feat)}]{mismatch}")
    spw_ra, spw_dec = cap["steps_per_worm"]["ra"], cap["steps_per_worm"]["dec"]
    print(f"Steps/worm(RA):   {spw_ra}")
    print(f"Steps/worm(Dec):  {spw_dec}")
    if ra_cpr and spw_ra:
        print(f"  -> worm teeth (RA):  {ra_cpr / spw_ra:.3f}")
    if dec_cpr and spw_dec:
        print(f"  -> worm teeth (Dec): {dec_cpr / spw_dec:.3f}")


def emit_cpp(cap: dict, name: str, capture_date: str, provenance: str) -> str:
    ra_cpr, dec_cpr = cap["cpr"]["ra"], cap["cpr"]["dec"]
    ra_tf, dec_tf = cap["timer_freq"]["ra"], cap["timer_freq"]["dec"]
    ra_feat, dec_feat = cap["features"]["ra"], cap["features"]["dec"]
    spw_ra = cap["steps_per_worm"]["ra"]

    cpr_note = "" if ra_cpr == dec_cpr else f"  // WARNING: RA={ra_cpr} Dec={dec_cpr}, using RA"
    tf_note = "" if ra_tf == dec_tf else f"  // WARNING: RA={ra_tf} Dec={dec_tf}, using RA"
    feat_note = "" if ra_feat == dec_feat else f"  // WARNING: RA=0x{ra_feat:04X} Dec=0x{dec_feat:04X}, using RA"
    feature_bits_comment = decode_feature_bits(ra_feat)
    worm_comment = ""
    if ra_cpr and spw_ra:
        worm_comment = f" ({ra_cpr}/{spw_ra} = {ra_cpr / spw_ra:.0f} worm teeth)"

    lines = [
        f"// Sky-Watcher {cap['model_name']}, MC firmware {cap['fw_string']}, mount code "
        f"0x{cap['mount_code']:02X}. Captured {provenance} on {capture_date}:",
        f"//   :e -> ={cap['version_reply']}   :a -> {ra_cpr}   :b -> {ra_tf}   :g -> {cap['high_speed_ratio_reply']}",
    ]
    if spw_ra:
        lines.append(f"//   :s -> {spw_ra}{worm_comment}")
    lines.append(f"//   :q 0x000001 -> 0x{ra_feat:04X}  {feature_bits_comment}")
    if 0x04 & ra_feat:
        lines.append("// HOME_INDEXER bit set -> CanFindHome should be true on this board.")
    else:
        lines.append("// No HOME_INDEXER bit (0x04) -> CanFindHome must be false on this board.")
    lines += [
        f"static FakeMountProfile {name}() {{",
        "    FakeMountProfile p;",
        f"    p.cpr = {ra_cpr};{cpr_note}",
        f"    p.timer_freq = {ra_tf};{tf_note}",
        f'    p.version_reply = "{cap["version_reply"]}";',
        f'    p.high_speed_ratio_reply = "{cap["high_speed_ratio_reply"]}";',
        f"    p.features = 0x{ra_feat:04X};{feat_note}",
    ]
    if spw_ra:
        lines.append(f"    p.steps_per_worm = {spw_ra};")
    lines += [
        "    return p;",
        "}",
    ]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    transport = parser.add_mutually_exclusive_group(required=True)
    transport.add_argument("--port", default=None, help="Serial device, e.g. /dev/ttyUSB0")
    transport.add_argument(
        "--alpaca-url",
        default=None,
        help="Base Telescope URL of a running AlpacaBridge instance, "
        "e.g. https://host/api/v1/telescope/0",
    )
    parser.add_argument("--baud", type=int, default=None, help="Serial only: skip auto-probe, use this baud directly")
    parser.add_argument("--client-id", type=int, default=1, help="Alpaca only: ClientID to use (default 1)")
    parser.add_argument("--timeout-ms", type=int, default=1000, help="Per-exchange timeout (default 1000)")
    parser.add_argument("--name", default=None, help="C++ function name to emit, e.g. eq_al55i")
    parser.add_argument("--date", default=None, help="Capture date for the comment (default: today, UTC)")
    parser.add_argument("-v", "--verbose", action="store_true", help="Log every command/reply to stderr")
    args = parser.parse_args()

    timeout_s = args.timeout_ms / 1000.0
    capture_date = args.date or datetime.datetime.now(datetime.timezone.utc).date().isoformat()

    alpaca_link = None
    try:
        if args.port:
            if args.baud is not None:
                import serial

                ser = serial.Serial(
                    args.port, baudrate=args.baud, bytesize=8, parity="N", stopbits=1, timeout=timeout_s
                )
                link = SerialLink(ser, verbose=args.verbose)
                baud = args.baud
            else:
                link, baud = probe_baud(args.port, timeout_s, args.verbose)
            port_desc = args.port
            provenance = "over the mount's own USB port / EQDIR cable (direct serial)"
        else:
            alpaca_link = AlpacaLink(
                args.alpaca_url, client_id=args.client_id, timeout_s=max(timeout_s, 5.0), verbose=args.verbose
            )
            alpaca_link.ensure_connected()
            link = alpaca_link
            baud = "n/a (Alpaca)"
            port_desc = args.alpaca_url
            provenance = f"via AlpacaBridge CommandString passthrough ({args.alpaca_url})"

        cap = capture(link)
    except MotorControllerError as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)
    finally:
        if alpaca_link is not None:
            alpaca_link.restore()

    print_summary(cap, port_desc, baud)

    name = args.name
    if not name:
        # Derive a reasonable default from the model name, e.g. "EQM-35 Pro" -> "eqm35_pro"
        name = "".join(c.lower() if c.isalnum() else "_" for c in cap["model_name"])
        while "__" in name:
            name = name.replace("__", "_")
        name = name.strip("_")

    print()
    print("=== FakeMountProfile (paste into AlpacaCore/tests/fake_skywatcher_mount.h) ===")
    print()
    print(emit_cpp(cap, name, capture_date, provenance))
    print()
    print(
        "Review every WARNING comment above before committing -- an axis mismatch means "
        "one FakeMountProfile field cannot represent both axes and the fixture needs a "
        "closer look, not a silent pick.",
        file=sys.stderr,
    )
    print()
    print("=== What to do with this ===", file=sys.stderr)
    print(
        "If a maintainer asked you to run this (e.g. to add a FakeMountProfile for your\n"
        "board): copy this ENTIRE terminal output, from '=== Capture summary ===' down,\n"
        "and paste it as a comment on the GitHub issue that asked for it (for example\n"
        "https://github.com/open-astro/AlpacaBridge/issues/306) -- or attach it to a new\n"
        "issue if you weren't pointed here from one. You do not need to edit any C++\n"
        "yourself; a maintainer will fold the block above into the test fixture.\n"
        "Nothing above was a motion command -- your mount did not move.",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
