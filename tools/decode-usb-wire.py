"""Decode bounded full-speed USB pin snapshots, not arbitrary live USB traffic."""

import argparse
import json
import math
from pathlib import Path
import re

PID_NAMES = {
    0xE1: "OUT", 0x69: "IN", 0xA5: "SOF", 0x2D: "SETUP",
    0xC3: "DATA0", 0x4B: "DATA1", 0xD2: "ACK", 0x5A: "NAK", 0x1E: "STALL",
}
FIRST_DESCRIPTOR = bytes.fromhex("80 06 00 01 00 00 08 00")
BEGIN = re.compile(
    r"\[usb-wire\] BEGIN (root|child) samples=(\d+) sys_hz=(\d+) "
    r"divider=(\d+) hub=(\d+) port=(\d+)(?: generation=(\d+) started_us=(\d+))?$"
)
DATA = re.compile(r"\[usb-wire\] DATA (root|child) offset=(\d+) hex=([0-9a-fA-F]+)$")
END = re.compile(r"\[usb-wire\] END (root|child)$")


def crc5(token):
    crc = 0x1F
    for bit in range(11):
        feedback = (crc ^ (token >> bit)) & 1
        crc >>= 1
        if feedback:
            crc ^= 0x14
    return crc ^ 0x1F


def crc16(payload):
    crc = 0xFFFF
    for value in payload:
        crc ^= value
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc ^ 0xFFFF


def read_captures(text):
    captures = []
    current = None
    for line in text.splitlines():
        marker = line.find("[usb-wire]")
        if marker < 0:
            continue
        line = line[marker:].strip()
        if "replay FAILED" in line:
            raise ValueError("UART dump reported a failure")
        match = BEGIN.fullmatch(line)
        if match:
            if current is not None:
                raise ValueError("new capture began before previous END")
            name, count, clock, divider, hub, port, generation, started = match.groups()
            count, clock, divider, hub, port = map(int, (count, clock, divider, hub, port))
            if not 0 < count <= 8192 or count % 4:
                raise ValueError("invalid sample count")
            if not 0 < divider <= 64 or not 0 < clock <= 600_000_000:
                raise ValueError("invalid sampling clock")
            if (name == "root" and (hub or port)) or (name == "child" and not (0 < hub <= 127 and 0 < port <= 255)):
                raise ValueError("capture name and USB topology disagree")
            generation = int(generation) if generation is not None else None
            started = int(started) if started is not None else None
            if generation is not None and (generation > 0xFFFFFFFF or started > 0xFFFFFFFFFFFFFFFF):
                raise ValueError("invalid capture generation/time")
            current = dict(name=name, count=count, clock=clock, divider=divider,
                           hub=hub, port=port, generation=generation, started_us=started,
                           packed=bytearray())
            continue
        match = DATA.fullmatch(line)
        if match:
            name, offset, encoded = match.groups()
            if current is None or name != current["name"] or int(offset) != len(current["packed"]):
                raise ValueError("missing, duplicate or out-of-order sample chunk")
            if len(encoded) % 2 or len(encoded) > 128:
                raise ValueError("invalid sample chunk size")
            current["packed"].extend(bytes.fromhex(encoded))
            if len(current["packed"]) * 4 > current["count"]:
                raise ValueError("too many samples")
            continue
        match = END.fullmatch(line)
        if match:
            if current is None or match[1] != current["name"]:
                raise ValueError("END without matching BEGIN")
            if len(current["packed"]) * 4 != current["count"]:
                raise ValueError("truncated sample dump")
            current["samples"] = [(value >> shift) & 3
                                  for value in current.pop("packed") for shift in (0, 2, 4, 6)]
            captures.append(current)
            current = None
        elif line.startswith(("[usb-wire] BEGIN", "[usb-wire] DATA", "[usb-wire] END")):
            raise ValueError("malformed wire dump record")
    if current is not None:
        raise ValueError("capture has no END")
    if not captures:
        raise ValueError("no complete wire captures found")
    return captures


def interpret_packet(raw):
    result = {"bytes": raw.hex(" "), "valid": False}
    if not raw:
        return dict(result, error="missing PID")
    pid = raw[0]
    result["pid"] = PID_NAMES.get(pid, f"unknown-{pid:02x}")
    if ((pid >> 4) ^ (pid & 15)) != 15:
        return dict(result, error="invalid PID complement")
    if pid in (0xE1, 0x69, 0xA5, 0x2D):
        if len(raw) != 3:
            return dict(result, error="invalid token length")
        token = int.from_bytes(raw[1:], "little")
        if crc5(token & 0x7FF) != token >> 11:
            return dict(result, error="token CRC5 mismatch")
        if pid == 0xA5:
            result["frame"] = token & 0x7FF
        else:
            result.update(address=token & 127, endpoint=(token >> 7) & 15)
    elif pid in (0xC3, 0x4B):
        if len(raw) < 3:
            return dict(result, error="truncated data packet")
        payload = raw[1:-2]
        result["payload"] = payload.hex(" ")
        if crc16(payload) != int.from_bytes(raw[-2:], "little"):
            return dict(result, error="data CRC16 mismatch")
    elif pid in (0xD2, 0x5A, 0x1E):
        if len(raw) != 1:
            return dict(result, error="invalid handshake length")
    else:
        return dict(result, error="unsupported full-speed PID")
    result["valid"] = True
    return result


def decode_samples(samples, sample_hz):
    if not samples or any(state not in (0, 1, 2, 3) for state in samples):
        raise ValueError("invalid digital line states")
    per_bit = sample_hz / 12_000_000
    if not math.isfinite(per_bit) or per_bit < 2:
        raise ValueError("nominal sample rate is below two samples per full-speed bit")
    packets = []
    index = 1
    while index < len(samples):
        # Pin snapshots can show one intermediate SE0/SE1 at the J->K edge.
        # Only widen SOP detection; never alter samples or relax packet checks.
        transition = (index >= 2 and samples[index - 2] == 1
                      and samples[index - 1] in (0, 3))
        if samples[index] != 2 or not (samples[index - 1] == 1 or transition):
            index += 1
            continue
        start = index
        # The observed edge is quantized to one sample; use its interval midpoint.
        position = start - 0.5 + per_bit / 2
        previous = 1
        bits = []
        error = None
        eop = False
        while round(position) < len(samples):
            state = samples[round(position)]
            if state == 0:
                second, idle = round(position + per_bit), round(position + 2 * per_bit)
                eop = idle < len(samples) and samples[second] == 0 and samples[idle] == 1
                if not eop:
                    error = "invalid or truncated EOP"
                position += 3 * per_bit
                break
            if state == 3:
                error = "invalid SE1 line state"
                break
            bits.append(int(state == previous))
            previous = state
            position += per_bit
        if not eop and error is None:
            error = "packet extends beyond capture window"
        if len(bits) < 8 or bits[:8] != [0, 0, 0, 0, 0, 0, 0, 1]:
            error = error or "SYNC mismatch"
        payload_bits = []
        ones = 1
        for bit in bits[8:]:
            if ones == 6:
                if bit:
                    error = error or "bit-stuff violation"
                ones = 0
                continue
            payload_bits.append(bit)
            ones = ones + 1 if bit else 0
        if ones == 6:
            error = error or "missing terminal stuffed bit"
        if len(payload_bits) % 8:
            error = error or "partial byte"
        raw = bytes(sum(bit << shift for shift, bit in enumerate(payload_bits[offset:offset + 8]))
                    for offset in range(0, len(payload_bits) - 7, 8))
        packet = interpret_packet(raw)
        if error:
            packet.update(valid=False, error=error)
        packet.update(start_sample=start, nominal_start_us=round(start / sample_hz * 1_000_000, 3))
        if transition:
            packet["sop_transition_samples"] = 1
        packets.append(packet)
        if eop:
            index = max(start + 1, math.floor(position))
        else:
            # Do not mistake transitions inside a damaged packet for new packets.
            index = start + 1
            while index < len(samples) and samples[index] != 0:
                index += 1
            while index < len(samples) and samples[index] == 0:
                index += 1
    return packets


def setup_observations(packets):
    observations = []
    for index in range(len(packets) - 1):
        token, data = packets[index:index + 2]
        if not (token["valid"] and token.get("pid") == "SETUP" and token.get("address") == 0
                and token.get("endpoint") == 0 and data["valid"] and data.get("pid") == "DATA0"
                and data.get("payload") == FIRST_DESCRIPTOR.hex(" ")):
            continue
        following = packets[index + 2] if index + 2 < len(packets) else None
        ack = bool(following and following["valid"] and following.get("pid") == "ACK")
        observations.append({"packet_index": index, "adjacent_ack_decoded": ack})
    return observations


def analyze(text):
    results = []
    for capture in read_captures(text):
        packets = decode_samples(capture["samples"], capture["clock"] / capture["divider"])
        observations = setup_observations(packets)
        results.append({
            "capture": capture["name"], "hub": capture["hub"], "port": capture["port"],
            "generation": capture["generation"], "started_us": capture["started_us"],
            "nominal_sample_hz": capture["clock"] / capture["divider"],
            "sample_count": capture["count"], "packets": packets, "setups": observations,
        })
    paired = len(results) >= 2 and results[-2]["capture"] == "root" and results[-1]["capture"] == "child"
    reference = paired and any(item["adjacent_ack_decoded"] for item in results[-2]["setups"])
    if paired and results[-2]["started_us"] is not None and results[-1]["started_us"] is not None:
        reference = reference and results[-2]["started_us"] <= results[-1]["started_us"]
    return {
        "root_reference_valid_for_last_child": bool(reference),
        "limitation": (
            "Nominal-rate digital sampling of Pico's upstream pins only, not an analog or downstream probe. "
            "Missing/invalid decoding is not proof of a bad USB packet or absent device. "
            "Validate the successful root SETUP/DATA0/ACK reference before interpreting the child window."
        ),
        "captures": results,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="UART log containing complete usb wire dumps")
    args = parser.parse_args()
    try:
        result = analyze(args.log.read_text(encoding="utf-8-sig", errors="strict"))
    except (OSError, UnicodeError, ValueError) as exc:
        parser.exit(2, f"USB wire decode failed: {exc}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
