import importlib.util
import math
from pathlib import Path
import unittest

path = Path(__file__).resolve().parents[1] / "tools" / "decode-usb-wire.py"
spec = importlib.util.spec_from_file_location("usb_wire_decode", path)
wire = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wire)


def token(pid=0x2D, address=0, endpoint=0):
    value = address | (endpoint << 7)
    return bytes([pid]) + (value | wire.crc5(value) << 11).to_bytes(2, "little")


def data_packet(payload):
    return b"\xc3" + payload + wire.crc16(payload).to_bytes(2, "little")


def waveform(packets, phase=0.25, clock=150_000_000, stuff=True):
    cells = [1] * 12
    for packet in packets:
        previous = 1
        ones = 0
        for value in b"\x80" + packet:
            for shift in range(8):
                bit = (value >> shift) & 1
                if not bit:
                    previous = 3 - previous
                cells.append(previous)
                ones = ones + 1 if bit else 0
                if stuff and ones == 6:
                    previous = 3 - previous
                    cells.append(previous)
                    ones = 0
        cells += [0, 0, 1] + [1] * 6
    per_bit = clock / 4 / 12_000_000
    samples = [cells[min(int((index + phase) / per_bit), len(cells) - 1)]
               for index in range(math.ceil(len(cells) * per_bit))]
    samples += [1] * ((-len(samples)) % 4)
    return samples


def dump(name, samples, clock=150_000_000):
    hub, port = (0, 0) if name == "root" else (5, 1)
    lines = [f"[usb-wire] BEGIN {name} samples={len(samples)} sys_hz={clock} divider=4 hub={hub} port={port}"]
    packed = bytes(sum(samples[index + shift] << (shift * 2) for shift in range(4))
                   for index in range(0, len(samples), 4))
    for offset in range(0, len(packed), 16):
        lines.append(f"[usb-wire] DATA {name} offset={offset} hex={packed[offset:offset + 16].hex()}")
    lines.append(f"[usb-wire] END {name}")
    return "\n".join(lines) + "\n"


class WireDecodeTests(unittest.TestCase):
    def test_known_crc_vectors(self):
        self.assertEqual(wire.crc16(b"123456789"), 0xB4C8)
        self.assertEqual(token(), bytes.fromhex("2d 00 10"))

    def test_setup_and_ack_across_sample_phases(self):
        packets = [token(), data_packet(wire.FIRST_DESCRIPTOR), b"\xd2"]
        for clock in (125_000_000, 150_000_000):
            for phase in (0.05, 0.25, 0.75, 0.95):
                with self.subTest(clock=clock, phase=phase):
                    decoded = wire.decode_samples(waveform(packets, phase, clock), clock / 4)
                    self.assertEqual([packet["pid"] for packet in decoded], ["SETUP", "DATA0", "ACK"])
                    self.assertTrue(all(packet["valid"] for packet in decoded), decoded)
                    self.assertTrue(wire.setup_observations(decoded)[0]["adjacent_ack_decoded"])

    def test_bit_stuffing_and_empty_data(self):
        for payload in (b"", b"\xff" * 8, bytes(range(8)), bytes.fromhex("ff f0 7f fe 00")):
            decoded = wire.decode_samples(waveform([data_packet(payload)]), 37_500_000)
            self.assertEqual(len(decoded), 1)
            self.assertTrue(decoded[0]["valid"], decoded)
            self.assertEqual(decoded[0]["payload"], payload.hex(" "))

    def test_invalid_pid_and_crc_are_rejected(self):
        valid_data = data_packet(wire.FIRST_DESCRIPTOR)
        invalid = [b"\xd3", bytes.fromhex("2d 00 18"), valid_data[:-1] + bytes([valid_data[-1] ^ 1])]
        for packet in invalid:
            self.assertFalse(wire.interpret_packet(packet)["valid"])
            self.assertFalse(wire.decode_samples(waveform([packet]), 37_500_000)[0]["valid"])

    def test_malformed_stuffing_sync_and_eop(self):
        samples = waveform([data_packet(b"\xff" * 8)], stuff=False)
        packet = wire.decode_samples(samples, 37_500_000)[0]
        self.assertFalse(packet["valid"])
        self.assertEqual(packet["error"], "bit-stuff violation")
        for state, expected in ((3, "invalid SE1"), (1, "SYNC mismatch")):
            samples = waveform([token()])
            samples[43:47] = [state] * 4
            packet = wire.decode_samples(samples, 37_500_000)[0]
            self.assertFalse(packet["valid"])
            self.assertIn(expected, packet["error"])
        samples = waveform([token()])
        first_se0 = samples.index(0)
        samples[first_se0 + 3:first_se0 + 7] = [2] * 4
        packet = wire.decode_samples(samples, 37_500_000)[0]
        self.assertFalse(packet["valid"])
        self.assertIn("EOP", packet["error"])

    def test_child_without_ack_is_not_success(self):
        request = [token(), data_packet(wire.FIRST_DESCRIPTOR)]
        text = dump("root", waveform(request + [b"\xd2"]))
        text += "[output] unrelated console message\n" + dump("child", waveform(request))
        result = wire.analyze(text)
        self.assertTrue(result["root_reference_valid_for_last_child"])
        self.assertFalse(result["captures"][1]["setups"][0]["adjacent_ack_decoded"])

    def test_missing_reference_is_not_validated(self):
        child = dump("child", waveform([token(), data_packet(wire.FIRST_DESCRIPTOR), b"\xd2"]))
        self.assertFalse(wire.analyze(child)["root_reference_valid_for_last_child"])
        idle = dump("root", [1] * 256)
        self.assertFalse(wire.analyze(idle + child)["root_reference_valid_for_last_child"])

    def test_corrupt_or_partial_uart_dump_is_rejected(self):
        complete = dump("root", [1] * 256)
        cases = [
            "", complete.replace("[usb-wire] END root", ""),
            complete.replace("offset=16", "offset=0"),
            complete.replace("samples=256", "samples=260"),
            complete.replace("hex=5555", "hex=gg55"),
            complete + "[usb-wire] UART replay FAILED (format/short write)\n",
        ]
        for text in cases:
            with self.assertRaises(ValueError):
                wire.read_captures(text)

    def test_capture_window_and_sample_rate_limits(self):
        samples = waveform([token(), data_packet(wire.FIRST_DESCRIPTOR)])
        samples = samples[:len(samples) - 30]
        decoded = wire.decode_samples(samples, 37_500_000)
        self.assertFalse(decoded[-1]["valid"])
        self.assertFalse(wire.setup_observations(decoded))
        with self.assertRaises(ValueError):
            wire.decode_samples([1, 2], 12_000_000)

    def test_rearmed_capture_metadata_and_legacy_logs(self):
        samples = waveform([token(), data_packet(wire.FIRST_DESCRIPTOR), b"\xd2"])
        legacy = dump("root", samples) + dump("child", samples)
        self.assertIsNone(wire.analyze(legacy)["captures"][1]["generation"])
        current = legacy.replace("hub=0 port=0", "hub=0 port=0 generation=0 started_us=1000")
        current = current.replace("hub=5 port=1", "hub=5 port=1 generation=2 started_us=9000000")
        result = wire.analyze(current)
        self.assertTrue(result["root_reference_valid_for_last_child"])
        self.assertEqual(result["captures"][1]["generation"], 2)
        self.assertEqual(result["captures"][1]["started_us"], 9000000)
        self.assertFalse(wire.analyze(current.replace("started_us=9000000", "started_us=100"))[
            "root_reference_valid_for_last_child"])
        with self.assertRaises(ValueError):
            wire.read_captures(current.replace("generation=2", "generation=4294967296"))

    def test_single_sample_transition_at_packet_start(self):
        packets = [token(), data_packet(wire.FIRST_DESCRIPTOR), b"\xd2"]
        samples = waveform(packets)
        original = wire.decode_samples(samples, 37_500_000)
        for packet_index in range(3):
            start = original[packet_index]["start_sample"]
            for intermediate in (0, 3):
                changed = samples.copy()
                changed[start - 1] = intermediate
                decoded = wire.decode_samples(changed, 37_500_000)
                self.assertEqual([packet["bytes"] for packet in decoded],
                                 [packet.hex(" ") for packet in packets])
                self.assertTrue(all(packet["valid"] for packet in decoded), decoded)
                self.assertEqual(decoded[packet_index]["sop_transition_samples"], 1)
                self.assertTrue(wire.setup_observations(decoded)[0]["adjacent_ack_decoded"])
                self.assertEqual(changed[start - 1], intermediate)
        changed = samples.copy()
        start = original[1]["start_sample"]
        changed[start - 2:start] = [3, 3]
        self.assertFalse(wire.setup_observations(wire.decode_samples(changed, 37_500_000)))

    def test_recorded_0420_root_reference(self):
        # Actual 0.4.20 root capture, samples 320..863 (no HID/user payload).
        packed = bytes.fromhex(
            "5555555555a9955aa995aaaa5655a5aad56aa5566aa556ab955aa96aa55e0140"
            "555555855aa9955aa9aaaa5aa995aaaa576aa5566a55adaa6aa5566ab55aa995"
            "5aa955955aa9955aa9576aa5566aa55ea9955aa9957aa556955aa9d56aa5566a"
            "a5565595aa565555ad5695aa56aa0a0054555555556aa556a995aa56955a55aa"
            "aa0a005555555555")
        samples = [(value >> shift) & 3 for value in packed for shift in (0, 2, 4, 6)]
        decoded = wire.decode_samples(samples, 37_500_000)
        self.assertEqual([packet["bytes"] for packet in decoded],
                         ["2d 00 10", "c3 80 06 00 01 00 00 08 00 eb 94", "d2"])
        self.assertTrue(all(packet["valid"] for packet in decoded), decoded)
        self.assertEqual(decoded[1]["sop_transition_samples"], 1)
        self.assertTrue(wire.setup_observations(decoded)[0]["adjacent_ack_decoded"])


if __name__ == "__main__":
    unittest.main()
