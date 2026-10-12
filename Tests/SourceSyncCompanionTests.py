# SPDX-License-Identifier: AGPL-3.0-only
"""Run with: python3 -m unittest discover -s Tests -p '*SourceSync*.py'."""
import importlib.util
from pathlib import Path
import sys
import types
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "FLStudio/PocketBalanceSourceSync/device_PocketBalanceSourceSync.py"
for name in ("mixer", "plugins", "general"):
    sys.modules[name] = types.ModuleType(name)
spec = importlib.util.spec_from_file_location("source_sync_companion", SCRIPT)
sync = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sync)


def context(track=20, slot=0, revision=11, linked=True):
    return (track + 1) | ((slot + 1) << 8) | (4096 if linked else 0) | (revision << 13)


class Receiver:
    """Independent wire decoder. ACK changes only when delivery is processed."""
    def __init__(self, ctx):
        self.context = ctx
        self.ack = 0
        self.mailbox = None
        self.history = []
        self.names = {}
        self.route_mask = 0
        self.active = None

    def write(self, normalized):
        self.mailbox = int(normalized * 16777216 + 0.5)
        self.history.append(self.mailbox)

    def read(self):
        return self.ack / 16777216

    def pump(self):
        if self.mailbox is None or self.mailbox == self.ack:
            return
        word = self.mailbox
        op, payload = (word >> 20) & 7, word & 0xfffff
        if op == 4:
            self.active = None
        elif op == 5:
            self.route_mask = payload & 65535
        elif op == 1:
            self.active = ((payload >> 16) & 15, (payload >> 7) & 511, payload & 127, bytearray(), 0)
        elif op == 2:
            lane, length, nonce, body, chunk = self.active
            if payload >> 16 != chunk % 16:
                raise AssertionError("Chunk order mismatch")
            body.append((payload >> 8) & 255)
            if len(body) < length:
                body.append(payload & 255)
            self.active = (lane, length, nonce, body, chunk + 1)
        elif op == 3:
            lane, length, nonce, body, chunk = self.active
            if len(body) != length or payload & 15 != nonce & 15:
                raise AssertionError("Invalid frame length or nonce")
            header = bytes([1, lane, length >> 8, length & 255, nonce,
                            self.context >> 16, (self.context >> 8) & 255, self.context & 255])
            # Independent bitwise division implementation of CRC-CCITT-FALSE.
            remainder = 65535
            for byte in header + body:
                for bit in range(7, -1, -1):
                    carry = bool(remainder & 32768) != bool(byte & (1 << bit))
                    remainder = (remainder * 2) & 65535
                    if carry:
                        remainder ^= 4129
            if remainder != payload >> 4:
                raise AssertionError("CRC mismatch")
            self.names[lane] = (body[0], body[1:].decode("utf-8"))
            self.active = None
        self.ack = word


def drive(link, receiver, start=0, count=200):
    for tick in range(start, start + count):
        link.step(tick)
        receiver.pump()


class ProtocolTests(unittest.TestCase):
    def test_crc_known_vector(self):
        self.assertEqual(sync.crc16(b"123456789"), 0x29B1)

    def test_all_words_round_trip_through_float32(self):
        import struct
        for word in (0, 1, 0x7fffff, 0x800000, 0xffffff, 0x912345):
            float32 = struct.unpack("f", struct.pack("f", word / sync.SCALE))[0]
            self.assertEqual(sync.decode_word(float32), word)
        for value in (-1, 1, float("nan"), float("inf"), 0.5 / sync.SCALE):
            with self.assertRaises(ValueError):
                sync.decode_word(value)

    def test_unicode_maximum_and_wraparound(self):
        receiver = Receiver(context())
        link = sync.Link(20, 0, {}, receiver.context, receiver.read, receiver.write)
        names = {0: (12, "Kick"), 7: (19, "鼓" * 64), 15: (24, "🥁" * 64)}
        link.configure(names, (1 << 0) | (1 << 7) | (1 << 15), 0)
        drive(link, receiver, count=350)
        self.assertEqual(receiver.names, names)
        self.assertEqual(link.delivered, names)
        self.assertEqual(receiver.route_mask, 32897)
        for previous, current in zip(receiver.history, receiver.history[1:]):
            self.assertNotEqual(previous & sync.PHASE, current & sync.PHASE)

    def test_encoder_has_fixed_wire_vector(self):
        # Exposes the agreed protocol without depending on the receiver mock.
        words = sync.encode_name(2, 14, "Hat", context())
        self.assertEqual(words[0], (1, (2 << 16) | (4 << 7) | 11))
        self.assertEqual(words[1:3], [(2, 0x0e48), (2, 0x16174)])
        expected_crc = sync.crc16(bytes([1, 2, 0, 4, 11, 1, 113, 21, 14, 72, 97, 116]))
        self.assertEqual(words[3], (3, (expected_crc << 4) | 11))

    def test_empty_name_and_character_limit(self):
        self.assertEqual(sync.source_text("  ", 13), "Mixer 13")
        self.assertEqual(sync.source_text("x\x00y", 2), "xy")
        self.assertEqual(len(sync.source_text("🥁" * 80, 2)), 64)
        with self.assertRaises(ValueError):
            sync.encode_name(16, 1, "Kick", context())
        with self.assertRaises(ValueError):
            sync.encode_name(0, 255, "Kick", context())

    def test_no_advance_until_ack_and_same_packet_retry(self):
        receiver = Receiver(context())
        link = sync.Link(20, 0, {}, receiver.context, receiver.read, receiver.write)
        link.configure({0: (12, "Kick")}, 1, 0)
        link.step(0)
        link.step(sync.RETRY_TICKS - 1)
        self.assertEqual(len(receiver.history), 1)
        link.step(sync.RETRY_TICKS)
        self.assertEqual(receiver.history[0], receiver.history[1])
        self.assertEqual(receiver.names, {})
        receiver.pump()
        drive(link, receiver, start=sync.RETRY_TICKS + 1)
        self.assertEqual(receiver.names, {0: (12, "Kick")})

    def test_name_is_delivered_only_after_end_ack(self):
        receiver = Receiver(context())
        link = sync.Link(20, 0, {}, receiver.context, receiver.read, receiver.write)
        link.configure({0: (12, "Kick")}, 1, 0)
        drive(link, receiver, count=6)
        link.step(6)
        self.assertEqual((receiver.mailbox >> 20) & 7, sync.END)
        self.assertEqual(link.delivered, {})
        for tick in range(7, 17):
            link.step(tick)
        self.assertEqual(link.delivered, {})
        receiver.pump()
        drive(link, receiver, start=17, count=2)
        self.assertEqual(link.delivered, {0: (12, "Kick")})

    def test_retry_limit_backs_off_then_recovers(self):
        receiver = Receiver(context())
        link = sync.Link(20, 0, {}, receiver.context, receiver.read, receiver.write)
        link.configure({0: (12, "Kick")}, 1, 0)
        for tick in range(100):
            link.step(tick)
        self.assertEqual(len(receiver.history), sync.MAX_RETRIES + 1)
        self.assertGreater(link.backoff_until, 100)
        drive(link, receiver, start=100)
        self.assertEqual(receiver.names, {0: (12, "Kick")})

    def test_route_removal_aborts_old_transfer(self):
        receiver = Receiver(context())
        link = sync.Link(20, 0, {}, receiver.context, receiver.read, receiver.write)
        link.configure({0: (12, "Kick"), 1: (14, "Snare")}, 3, 0)
        drive(link, receiver, count=4)
        link.configure({1: (14, "Snare")}, 2, 4)
        drive(link, receiver, start=4)
        self.assertNotIn(0, receiver.names)
        self.assertEqual(receiver.names[1], (14, "Snare"))
        self.assertEqual(receiver.route_mask, 2)

    def test_rename_during_transfer_sends_latest_only(self):
        receiver = Receiver(context())
        link = sync.Link(20, 0, {}, receiver.context, receiver.read, receiver.write)
        link.configure({0: (12, "Old Kick")}, 1, 0)
        drive(link, receiver, count=4)
        link.configure({0: (12, "New Kick")}, 1, 4)
        drive(link, receiver, start=4)
        self.assertEqual(receiver.names[0], (12, "New Kick"))

    def test_periodic_refresh_recovers_a_manual_label_change(self):
        receiver = Receiver(context())
        link = sync.Link(20, 0, {}, receiver.context, receiver.read, receiver.write)
        link.configure({0: (12, "Kick")}, 1, 0)
        drive(link, receiver, count=30)
        receiver.names[0] = (12, "Manual label")
        drive(link, receiver, start=30, count=sync.FULL_REFRESH_TICKS)
        self.assertEqual(receiver.names[0], (12, "Kick"))

    def test_unchanged_names_only_send_bounded_heartbeat(self):
        receiver = Receiver(context())
        link = sync.Link(20, 0, {}, receiver.context, receiver.read, receiver.write)
        link.configure({0: (12, "Kick")}, 1, 0)
        drive(link, receiver, count=30)
        before = len(receiver.history)
        drive(link, receiver, start=30, count=60)
        self.assertEqual(before, len(receiver.history))
        drive(link, receiver, start=90, count=110)
        self.assertTrue(all((word >> 20) & 7 in (0, 5) for word in receiver.history[before:]))
        self.assertLessEqual(len(receiver.history) - before, 4)


class FakeHost:
    def __init__(self):
        self.instances = {}
        self.routes = set()
        self.track_names = {12: "Kick", 13: "Closed Hat", 14: "Tom", 18: "808"}
        self.writes = []
        self.param_reads = []
        self.safe = True
        self.add(20, 0, {0: 12, 3: 14, 7: 18})
        sync.mixer.trackCount = lambda: 24
        sync.mixer.getTrackName = lambda track: self.track_names.get(track, "Insert " + str(track))
        sync.mixer.getRouteSendActive = lambda source, dest: (source, dest) in self.routes
        sync.plugins.isValid = lambda track, slot: (track, slot) in self.instances
        sync.plugins.getPluginName = lambda track, slot, user=0: self.instances[(track, slot)]["name"]
        sync.plugins.getParamCount = lambda track, slot: 4240
        sync.plugins.getParamName = self.param_name
        sync.plugins.getParamValue = self.param_value
        sync.plugins.setParamValue = self.set_value
        sync.general.safeToEdit = lambda: self.safe

    def add(self, track, slot, mapping, revision=11):
        # Model FL's inserted bypass, so discovery cannot use raw JUCE indices.
        names = ["Audio parameter " + str(n) for n in range(74)]
        names += [sync.PACKET_NAME, sync.ACK_NAME, sync.CONTEXT_NAME] + list(sync.MAP_NAMES)
        self.instances[(track, slot)] = {"name": "Pocket Balance", "names": names,
            "context": context(track, slot, revision), "mapping": dict(mapping),
            "receiver": Receiver(context(track, slot, revision))}
        self.routes.update((source, track) for source in mapping.values())

    def param_name(self, index, track, slot):
        self.param_reads.append(index)
        names = self.instances[(track, slot)]["names"]
        return names[index] if index < len(names) else "MIDI CC"

    def param_value(self, index, track, slot):
        instance = self.instances[(track, slot)]
        name = instance["names"][index]
        if name == sync.CONTEXT_NAME:
            return instance["context"] / sync.SCALE
        if name == sync.ACK_NAME:
            return instance["receiver"].read()
        if name in sync.MAP_NAMES:
            return (instance["mapping"].get(sync.MAP_NAMES.index(name), -1) + 1) / 256
        raise AssertionError("Companion read an audio parameter")

    def set_value(self, value, index, track, slot, pickup):
        instance = self.instances[(track, slot)]
        if instance["names"][index] != sync.PACKET_NAME:
            raise AssertionError("Companion wrote an audio parameter")
        self.writes.append((track, slot, index, value))
        instance["receiver"].write(value)

    def drive(self, companion, count=240):
        for unused in range(count):
            companion.idle()
            for instance in self.instances.values():
                instance["receiver"].pump()


class HostTests(unittest.TestCase):
    def test_explicit_gaps_and_two_instances(self):
        host = FakeHost()
        host.add(21, 1, {2: 13}, revision=27)
        companion = sync.Companion()
        host.drive(companion)
        self.assertEqual(host.instances[(20, 0)]["receiver"].names,
                         {0: (12, "Kick"), 3: (14, "Tom"), 7: (18, "808")})
        self.assertEqual(host.instances[(21, 1)]["receiver"].names, {2: (13, "Closed Hat")})
        self.assertLess(max(host.param_reads), sync.PARAMETER_LIMIT)
        self.assertEqual(set(write[2] for write in host.writes), {74})

    def test_mixer_rename_and_inactive_route(self):
        host = FakeHost()
        companion = sync.Companion()
        host.drive(companion)
        host.track_names[12] = "Kick revised"
        host.routes.remove((14, 20))
        companion.request_refresh()
        host.drive(companion, 100)
        receiver = host.instances[(20, 0)]["receiver"]
        self.assertEqual(receiver.names[0], (12, "Kick revised"))
        self.assertEqual(receiver.route_mask, (1 << 0) | (1 << 7))

    def test_context_change_and_project_reload_discard_old_stream(self):
        host = FakeHost()
        companion = sync.Companion()
        host.drive(companion, 55)
        self.assertTrue(host.writes)
        companion.clear(loading=True)
        count = len(host.writes)
        host.drive(companion, 20)
        self.assertEqual(count, len(host.writes))
        host.add(20, 0, {5: 13}, revision=45)
        companion.clear()
        host.drive(companion)
        self.assertEqual(host.instances[(20, 0)]["receiver"].names, {5: (13, "Closed Hat")})

    def test_replaced_plugin_never_receives_metadata_writes(self):
        host = FakeHost()
        companion = sync.Companion()
        host.drive(companion, 55)
        self.assertTrue(host.writes)
        host.instances[(20, 0)]["name"] = "Other Synth"
        count = len(host.writes)
        host.drive(companion, 150)
        self.assertEqual(len(host.writes), count)

    def test_more_instances_than_idle_budget_all_make_progress(self):
        host = FakeHost()
        for slot in range(1, 7):
            host.add(20, slot, {slot: 13}, revision=30 + slot)
        companion = sync.Companion()
        host.drive(companion, 350)
        for slot in range(1, 7):
            self.assertEqual(host.instances[(20, slot)]["receiver"].names, {slot: (13, "Closed Hat")})

    def test_wrong_owner_unlinked_and_unsafe_host_do_not_write(self):
        host = FakeHost()
        companion = sync.Companion()
        host.instances[(20, 0)]["context"] = context(21, 0)
        host.drive(companion)
        self.assertEqual(host.writes, [])
        host.instances[(20, 0)]["context"] = context(linked=False)
        host.drive(companion)
        self.assertEqual(host.writes, [])
        host.instances[(20, 0)]["context"] = context()
        host.safe = False
        host.drive(companion)
        self.assertEqual(host.writes, [])

    def test_callbacks_suspend_during_load_and_consume_dedicated_midi(self):
        host = FakeHost()
        sync.OnInit()
        sync.OnProjectLoad(0)
        self.assertTrue(sync._companion.loading)
        sync.OnProjectLoad(100)
        self.assertFalse(sync._companion.loading)
        event = types.SimpleNamespace(handled=False)
        sync.OnMidiIn(event)
        self.assertTrue(event.handled)
        sync.OnDeInit()
        self.assertTrue(sync._companion.loading)


if __name__ == "__main__":
    unittest.main()
