# name=Pocket Balance Source Sync
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright 2026 Pocket Balance contributors
"""FL Studio source-name companion. No files, networking, or audio changes.

Uses explicit mappings saved by Pocket Balance. It never infers input order.
Transport uses exactly representable 24-bit values and acknowledged packets.
"""
import mixer
import plugins
import general
import transport

SCALE = 16777216
PHASE = 8388608
MAX_WORD = SCALE - 1
NOP, BEGIN, DATA, END, RESET, STATUS = range(6)
PACKET_NAME = "source_sync_packet"
ACK_NAME = "source_sync_ack"
CONTEXT_NAME = "source_sync_context"
MAP_NAMES = tuple("source_sync_map" + str(n + 1) for n in range(16))
PARAMETER_LIMIT = 110
SLOT_COUNT = 10
MAX_TRACK = 500
KNOWN_POLL_TICKS = 30
SCAN_BATCH = 4
LINK_BATCH = 4
RETRY_TICKS = 8
MAX_RETRIES = 3
BACKOFF_TICKS = 90
HEARTBEAT_TICKS = 90
FULL_REFRESH_TICKS = 900
SNAPSHOT_TICKS = 12
SCAN_PAUSE_TICKS = 90


def decode_word(value):
    """Reject invalid host values instead of turning them into metadata."""
    if not isinstance(value, (int, float)) or not (0 <= value < 1):
        raise ValueError("Invalid source-sync value")
    word = int(value * SCALE + 0.5)
    if word > MAX_WORD or abs(value * SCALE - word) > 0.125:
        raise ValueError("Inexact source-sync value")
    return word


def decode_mapping(value):
    if not isinstance(value, (int, float)) or not (0 <= value < 1):
        raise ValueError("Invalid source mapping")
    scaled = value * 1024
    raw = int(scaled + 0.5)
    if raw > MAX_TRACK + 1 or abs(raw - scaled) > 0.0001:
        raise ValueError("Inexact source mapping")
    return raw - 1


def context_owner(context):
    return (context & 1023) - 1, ((context >> 10) & 15) - 1


def context_is_linked(context):
    return bool(context & 16384)


def crc16(data):
    crc = 65535
    for byte in data:
        crc ^= byte << 8
        for unused in range(8):
            crc = ((crc << 1) ^ 0x1021) & 65535 if crc & 32768 else (crc << 1) & 65535
    return crc


def source_text(name, source):
    # Sync preserves host spacing and uses the protocol's 256-byte name limit.
    text = str(name).replace("\x00", "")
    if not text.strip():
        return "Mixer " + str(source)
    # A UTF-8 suffix can be incomplete after truncation. Drop only that suffix.
    return text.encode("utf-8", "replace")[:256].decode("utf-8", "ignore")


def encode_name(lane, source, name, context):
    """Return (opcode, payload) pairs. The sender adds alternating phase bits."""
    if not 0 <= lane < 16 or not 0 <= source <= MAX_TRACK or not 0 <= context < PHASE:
        raise ValueError("Source-sync identity is out of range")
    encoded = source_text(name, source).encode("utf-8", "replace")
    body = bytes([source >> 8, source & 255]) + encoded
    length = len(body)
    if length < 3 or length > 258:
        raise ValueError("Source label is too long")
    nonce = (context >> 15) & 127
    header = bytes([1, lane, length >> 8, length & 255, nonce,
                    (context >> 16) & 255, (context >> 8) & 255, context & 255])
    result = [(BEGIN, (lane << 16) | (length << 7) | nonce)]
    for offset in range(0, length, 2):
        first = body[offset]
        second = body[offset + 1] if offset + 1 < length else 0
        result.append((DATA, (((offset // 2) & 15) << 16) | (first << 8) | second))
    result.append((END, (crc16(header + body) << 4) | (nonce & 15)))
    return result


class Link:
    """One instance, one outstanding packet. Retries never advance the stream."""
    def __init__(self, track, slot, indices, context, read_ack, write_packet):
        self.track, self.slot = track, slot
        self.indices, self.context = indices, context
        self.read_ack, self.write_packet = read_ack, write_packet
        self.phase = 0
        self.pending = None
        self.frame = []
        self.active_name = None
        self.desired = {}
        self.delivered = {}
        self.valid_mask = 0
        self.reset_needed = True
        self.status_needed = True
        self.last_activity = -HEARTBEAT_TICKS
        self.backoff_until = 0
        self.snapshot_at = -SNAPSHOT_TICKS
        self.full_refresh_at = 0

    def configure(self, names, valid_mask, tick):
        # Cancel a partially sent old name or a removed route before continuing.
        active_changed = self.active_name is not None and names.get(self.active_name[0]) != self.active_name[1:]
        if valid_mask != self.valid_mask or active_changed:
            self.pending = None
            self.frame = []
            self.active_name = None
            self.reset_needed = True
            self.status_needed = True
        self.desired = dict(names)
        self.valid_mask = valid_mask
        self.delivered = {lane: value for lane, value in self.delivered.items()
                          if names.get(lane) == value}
        self.snapshot_at = tick

    def _restart(self, tick):
        self.pending = None
        self.frame = []
        self.active_name = None
        self.delivered.clear()
        self.reset_needed = True
        self.status_needed = True
        self.backoff_until = tick + BACKOFF_TICKS

    def _command(self, tick):
        if self.reset_needed:
            self.reset_needed = False
            return RESET, 0, None
        if self.status_needed:
            self.status_needed = False
            return STATUS, self.valid_mask, None
        if not self.frame:
            for lane in sorted(self.desired):
                value = self.desired[lane]
                if self.delivered.get(lane) != value:
                    self.active_name = (lane,) + value
                    self.frame = encode_name(lane, value[0], value[1], self.context)
                    break
        if self.frame:
            opcode, payload = self.frame.pop(0)
            completed = self.active_name if opcode == END else None
            return opcode, payload, completed
        if tick - self.last_activity >= HEARTBEAT_TICKS:
            # Refresh route health even when every name stays unchanged.
            self.status_needed = True
            return NOP, 0, None
        return None

    def step(self, tick):
        if tick - self.full_refresh_at >= FULL_REFRESH_TICKS and self.pending is None and not self.frame:
            self.delivered.clear()
            self.full_refresh_at = tick
        ack = decode_word(self.read_ack())
        if self.pending is not None:
            word, sent_at, retries, completed = self.pending
            if ack == word:
                self.pending = None
                if completed is not None:
                    self.delivered[completed[0]] = completed[1:]
                    self.active_name = None
            elif tick - sent_at >= RETRY_TICKS:
                if retries >= MAX_RETRIES:
                    self._restart(tick)
                else:
                    self.write_packet(word / SCALE)
                    self.pending = (word, tick, retries + 1, completed)
                return
            else:
                return
        if tick < self.backoff_until:
            return
        command = self._command(tick)
        if command is None:
            return
        opcode, payload, completed = command
        # Read the receiver phase after RESET/retry recovery, not a cached guess.
        self.phase = (ack & PHASE) ^ PHASE
        word = self.phase | (opcode << 20) | payload
        self.write_packet(word / SCALE)
        self.pending = (word, tick, 0, completed)
        self.last_activity = tick


class Companion:
    def __init__(self):
        self.links = {}
        self.known_slots = {}
        self.focus_after = 0
        self.tick = 0
        self.scan_cursor = 0
        self.scan_after = 0
        self.refresh_requested = True
        self.loading = False
        self.recording = False
        self.link_cursor = 0

    def clear(self, loading=False):
        self.links.clear()
        self.known_slots.clear()
        self.focus_after = 0
        self.scan_cursor = 0
        self.scan_after = 0
        self.refresh_requested = True
        self.loading = loading
        self.recording = False

    def request_refresh(self):
        # Never rewind an ongoing scan: parameter ACKs can trigger OnRefresh.
        self.refresh_requested = True
        for link in self.links.values():
            link.snapshot_at = -SNAPSHOT_TICKS

    @staticmethod
    def _indices(track, slot):
        names = {}
        for index in range(min(PARAMETER_LIMIT, plugins.getParamCount(track, slot))):
            name = plugins.getParamName(index, track, slot)
            if name in (PACKET_NAME, ACK_NAME, CONTEXT_NAME) or name in MAP_NAMES:
                if name in names:
                    return None
                names[name] = index
        required = (PACKET_NAME, ACK_NAME, CONTEXT_NAME) + MAP_NAMES
        return names if all(name in names for name in required) else None

    @staticmethod
    def _read_context(track, slot, indices):
        return decode_word(plugins.getParamValue(indices[CONTEXT_NAME], track, slot))

    @staticmethod
    def _write_packet(value, index, track, slot):
        # Check again at the setter boundary. Metadata is carried through host
        # parameters and must not become a recorded automation performance.
        if transport.isRecording() or not general.safeToEdit():
            raise RuntimeError("Source-name updates paused")
        plugins.setParamValue(value, index, track, slot, 0)

    def _discover(self, track, slot):
        key = (track, slot)
        if not plugins.isValid(track, slot) or plugins.getPluginName(track, slot, 0) != "Pocket Balance":
            self.links.pop(key, None)
            self.known_slots.pop(key, None)
            return
        # Remember unlinked instances so Link does not wait for a full sweep.
        self.known_slots[key] = self.tick + KNOWN_POLL_TICKS
        indices = self._indices(track, slot)
        if indices is None:
            self.links.pop(key, None)
            return
        context = self._read_context(track, slot, indices)
        if not context_is_linked(context) or context_owner(context) != key:
            self.links.pop(key, None)
            return
        previous = self.links.get(key)
        if previous is not None and previous.context == context and previous.indices == indices:
            return
        self.links[key] = Link(track, slot, indices, context,
            lambda: plugins.getParamValue(indices[ACK_NAME], track, slot),
            lambda value: self._write_packet(value, indices[PACKET_NAME], track, slot))

    def _try_discover(self, key):
        try:
            self._discover(*key)
        except (ValueError, RuntimeError, TypeError, IndexError):
            self.links.pop(key, None)
            if key in self.known_slots:
                self.known_slots[key] = self.tick + KNOWN_POLL_TICKS

    def _scan(self):
        # Reserve two discovery slots for focused/known instances and retain
        # two for the background sweep. Neither path can starve the other.
        priority = []
        if self.tick >= self.focus_after:
            self.focus_after = self.tick + KNOWN_POLL_TICKS
            focus = mixer.getActiveEffectIndex()
            if (isinstance(focus, (tuple, list)) and len(focus) == 2
                    and 0 <= focus[0] <= MAX_TRACK and 0 <= focus[1] < SLOT_COUNT):
                key = tuple(focus)
                if self.known_slots.get(key, 0) <= self.tick:
                    priority.append(key)
        due = sorted((when, key) for key, when in self.known_slots.items() if when <= self.tick)
        for unused, key in due:
            if key not in priority:
                priority.append(key)
            if len(priority) >= 2:
                break
        for key in priority:
            self._try_discover(key)
        if self.tick < self.scan_after and not self.refresh_requested:
            return
        # trackCount includes Master and FL's Current utility track. The
        # protocol supports numbered inserts 0..500, without the utility track.
        total = max(0, min(MAX_TRACK + 1, mixer.trackCount())) * SLOT_COUNT
        for unused in range(SCAN_BATCH - len(priority)):
            if self.scan_cursor >= total:
                self.scan_cursor = 0
                self.scan_after = self.tick + SCAN_PAUSE_TICKS
                self.refresh_requested = False
                return
            position = self.scan_cursor
            self.scan_cursor += 1
            key = (position // SLOT_COUNT, position % SLOT_COUNT)
            if self.known_slots.get(key, 0) <= self.tick:
                self._try_discover(key)

    def _snapshot(self, link):
        context = self._read_context(link.track, link.slot, link.indices)
        if context != link.context or not context_is_linked(context) or context_owner(context) != (link.track, link.slot):
            return False
        mappings = [decode_mapping(plugins.getParamValue(link.indices[name], link.track, link.slot))
                    for name in MAP_NAMES]
        names = {}
        mask = 0
        count = min(MAX_TRACK + 1, mixer.trackCount())
        for lane, source in enumerate(mappings):
            if 0 <= source < count and source != link.track and mixer.getRouteSendActive(source, link.track):
                names[lane] = (source, source_text(mixer.getTrackName(source), source))
                mask |= 1 << lane
        if context != self._read_context(link.track, link.slot, link.indices):
            return False
        link.configure(names, mask, self.tick)
        return True

    def idle(self):
        self.tick += 1
        if self.loading:
            return
        if transport.isRecording():
            # Drop partially sent frames, while keeping known instance slots.
            self.links.clear()
            self.recording = True
            return
        if self.recording:
            self.recording = False
            self.links.clear()
            self.known_slots = {key: 0 for key in self.known_slots}
            self.focus_after = 0
            self.scan_after = 0
            self.request_refresh()
        if not general.safeToEdit():
            return
        self._scan()
        current = list(self.links.items())
        if not current:
            return
        count = min(LINK_BATCH, len(current))
        start = self.link_cursor % len(current)
        self.link_cursor = (start + count) % len(current)
        for offset in range(count):
            key, link = current[(start + offset) % len(current)]
            try:
                # Check parameter identity before writes. A replacement plugin
                # at the same slot must never receive packets as audio values.
                if (not plugins.isValid(*key)
                        or plugins.getPluginName(key[0], key[1], 0) != "Pocket Balance"
                        or plugins.getParamName(link.indices[PACKET_NAME], *key) != PACKET_NAME
                        or plugins.getParamName(link.indices[CONTEXT_NAME], *key) != CONTEXT_NAME
                        or self._read_context(*key, link.indices) != link.context):
                    # Re-read this exact slot now. Do not wait for the scanner
                    # to traverse hundreds of other Mixer tracks after relink.
                    self._try_discover(key)
                    continue
                active = link.active_name
                route_removed = active is not None and not mixer.getRouteSendActive(active[1], link.track)
                if route_removed or self.tick - link.snapshot_at >= SNAPSHOT_TICKS:
                    if not self._snapshot(link):
                        self._try_discover(key)
                        continue
                link.step(self.tick)
            except (ValueError, RuntimeError, TypeError, IndexError):
                self.links.pop(key, None)
                self.refresh_requested = True


_companion = Companion()


def OnInit():
    _companion.clear()
    print("Pocket Balance Source Sync ready. Set explicit input mappings in Pocket Balance.")


def OnDeInit():
    _companion.clear(loading=True)


def OnIdle():
    _companion.idle()


def OnDirtyMixerTrack(index):
    _companion.request_refresh()


def OnRefresh(flags):
    # Control-value notifications include our ACKs. Poll those at the normal
    # rate; mixer display/name changes request an earlier name read.
    if flags & (2 | 4 | 2048 | 16384):
        _companion.request_refresh()


def OnProjectLoad(status):
    _companion.clear(loading=status < 100)


def OnMidiIn(event):
    # This port is dedicated to metadata. It must not trigger instruments.
    event.handled = True
