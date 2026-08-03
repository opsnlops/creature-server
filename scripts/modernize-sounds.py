#!/usr/bin/env python3
"""
Modernize legacy animation sound files to the 48 kHz / S16 / 17-channel WAV format.

Refs opsnlops/creature-server#116.

The show's audio contract is a 48 kHz, signed-16-bit, 17-channel interleaved WAV:
channels 1-16 are creature lanes (creature.audio_channel is 1-based, so lane N
lives at interleaved index N-1) and channel 17 is the background-music lane.
`AudioStreamBuffer::loadFromWavFile` hard-rejects anything else, and because it
runs on the loader thread *after* the HTTP 200 has gone out, a legacy file shows
up as a dead playback session rather than an API error.

This walks the sounds on the server, probes each one, and brings the stragglers
up to spec. Files already matching the contract are left alone.

What can happen to an animation's audio:

  convert  The audio predates the contract (MP3, 44.1 kHz, 1- or 2-channel).
           ffmpeg downmixes it to mono at 48 kHz and it is placed on the lanes
           of the creatures the animation drives.

  repoint  The audio is already correct but the animation points at the wrong
           name (an MP3 that now has a WAV beside it, or a file that was
           renamed), or its multitrack_audio flag is stale.

  stamp    The audio is correct but the sidecar metadata hasn't been folded in.

Why the legacy audio can't simply be split L/R: those stereo files are one mono
mix of every voice duplicated to both channels -- cross-correlation is 1.000000
at lag 0 -- carrying only a binary +/-3 dB level flip that tracks who is
speaking. Both birds are at full strength in both channels, so panning recovers
nothing. (An earlier pass measured 0.91-0.998 and called them "ordinary stereo
mixes"; that was resampler noise, and the real figure is 1.000000.)

Separation instead comes from the animation itself. Each creature's track
carries mouth-axis bytes at its `mouth_slot`, so the creature whose mouth is
moving is the one talking. `--gate` uses that to route each creature's lines to
its own lane -- validated against the +/-3 dB flip, which agreed on every
segment with the silent creature's mouth axis at exactly 0.00 stdev, and which
also fixed the polarity: louder-left is Beaky (lane 1), louder-right is Mango
(lane 2). Audio no creature claims (breaths, music, applause) stays on lanes 1
and 2. Gating needs two creatures; a single-creature animation already lands on
the right lane.

`--standalone` does the sounds no animation references, which the console can
still trigger and which therefore hit the same loader. With no tracks to gate
on, the lane comes from a `beaky_`/`mango_` filename prefix, from the level flip
when the file carries a clean one, or failing that from DEFAULT_STANDALONE_LANE.
Superseded MP3/FLAC originals are moved to the backup dir so the console's list
only offers files that can actually play; a variant that is a materially
different take is left alone rather than filed away with no playable
counterpart. `--lanes` re-cuts a named file onto different lanes when the guess
was wrong.

Sidecar merge: older sounds carry their metadata in loose files beside the WAV --
a Rhubarb `.json` of mouth cues and a `.txt` transcript. Modern renders instead
embed all of it in the WAV itself as an iXML chunk. Where sidecars exist this
folds them into the WAV as `<USER><LIPSYNC>` cues and a `<DIALOG_SCRIPT>` block,
matching what `buildIxml` emits, so the file becomes self-describing. The
sidecars are left on disk; nothing is deleted.

Originals are backed up OUTSIDE the sounds root. That matters:
`SoundPathResolver::findByBasename` returns the first recursive basename match
under the root, so a backup kept inside `sounds/` would shadow the converted file
and quietly serve the stale one. Anything re-cut reads from that backup, since
re-reading the live file would downmix an already-17-channel mix a second time.

Every touched animation is re-uploaded through POST /api/v1/animation so the
Animation + SoundList cache invalidations reach connected clients; standalone
runs broadcast a SoundList invalidation directly, as no animation would.

Mouth motion is baked into track data at `mouth_slot` and is not derived from
audio at playback time, so moving audio between lanes does not disturb an
existing animation. It does affect a future lipsync *regeneration*, which
extracts the creature's own lane.

Usage:
    ./scripts/modernize-sounds.py                    # dry run, shows the plan
    ./scripts/modernize-sounds.py --apply            # convert, stamp, update
    ./scripts/modernize-sounds.py --gate --apply     # re-cut, one lane per speaker
    ./scripts/modernize-sounds.py --standalone --apply
    ./scripts/modernize-sounds.py --standalone --only 1997 --lanes 1,2 --apply
"""

import argparse
import array
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import urllib.error
import urllib.request

# --- The audio contract (mirrors src/server/config.h) ------------------------

TARGET_RATE = 48000
TARGET_CHANNELS = 17  # 16 creature lanes + 1 BGM lane
TARGET_BITS = 16
TARGET_CODEC = "pcm_s16le"
BGM_LANE = 17

# Only the two physical birds get legacy audio. Anything else in an animation's
# track list is ignored for the purposes of lane placement.
ALLOWED_LANES = (1, 2)

# AudioStreamBuffer refuses a decoded PCM payload larger than this, so a
# converted file above it would load but never play.
MAX_DECODED_PCM_BYTES = 512 * 1024 * 1024

DEFAULT_SERVER = "https://server.prod.chirpchirp.dev"
DEFAULT_SOUNDS_DIR = "/Volumes/creatures/sounds"
DEFAULT_BACKUP_DIR = "/Volumes/creatures/sounds-backup"

# Animations pointing at a file that no longer exists on disk, where a rename is
# unambiguous. The animation is repointed at the surviving file.
RENAMES = {
    "mfm2024-6-2-1-rule.wav": "mfm2024-six-two-one-rule.wav",
}

# Written into the iXML <NOTE> of a speaker-gated file. Doubles as the marker
# that tells a later run the file has already been split per speaker.
GATED_NOTE = "Speaker-gated legacy conversion to 17-channel"
PLAIN_NOTE = "Legacy conversion to 17-channel"
# A file whose iXML NOTE starts with one of these was written by this script and
# may be rewritten. Anything else -- notably the server's own dialog renders --
# is left strictly alone.
OWNED_NOTE_PREFIXES = (GATED_NOTE, PLAIN_NOTE)

# Placeholder text some old .txt sidecars carry instead of a real transcript.
BOILERPLATE_TRANSCRIPTS = {
    "Hello! This is an automatically generated file!",
}


# --- Talking to the server ---------------------------------------------------


def get_json(url):
    with urllib.request.urlopen(url, timeout=60) as response:
        return json.load(response)


def post_json(url, payload):
    # Posted verbatim. The animation DTO serializes unset strings as JSON null,
    # which the reader used to choke on (#117) -- that's fixed on main by
    # jsonStringOr(), so a GET -> POST round-trip is lossless and needs no
    # client-side massaging.
    body = json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(
        url, data=body, headers={"Content-Type": "application/json"}, method="POST"
    )
    try:
        with urllib.request.urlopen(request, timeout=180) as response:
            return json.load(response)
    except urllib.error.HTTPError as error:
        detail = error.read().decode("utf-8", errors="replace")[:400]
        raise RuntimeError(f"HTTP {error.code}: {detail}") from None


# --- Probing -----------------------------------------------------------------


def probe(path):
    """Return (codec, rate, channels) for `path`, or None if it can't be read."""
    result = subprocess.run(
        [
            "ffprobe", "-v", "error",
            "-select_streams", "a:0",
            "-show_entries", "stream=codec_name,sample_rate,channels",
            "-of", "csv=p=0",
            path,
        ],
        capture_output=True,
        text=True,
    )
    line = result.stdout.strip().split("\n")[0] if result.stdout.strip() else ""
    parts = line.split(",")
    if len(parts) < 3:
        return None
    try:
        return parts[0], int(parts[1]), int(parts[2])
    except ValueError:
        return None


def is_current_format(info):
    return info == (TARGET_CODEC, TARGET_RATE, TARGET_CHANNELS)


def duration_seconds(path):
    result = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration",
         "-of", "csv=p=0", path],
        capture_output=True,
        text=True,
    )
    try:
        return float(result.stdout.strip())
    except ValueError:
        return 0.0


# --- RIFF plumbing -----------------------------------------------------------


def riff_chunk_ids(path):
    """The four-character ids of every top-level chunk, in file order."""
    ids = []
    try:
        with open(path, "rb") as handle:
            if handle.read(4) != b"RIFF":
                return ids
            handle.seek(12)
            while True:
                header = handle.read(8)
                if len(header) < 8:
                    break
                chunk_id = header[:4]
                size = struct.unpack("<I", header[4:8])[0]
                ids.append(chunk_id.decode("ascii", errors="replace"))
                handle.seek(size + (size & 1), 1)
    except OSError:
        pass
    return ids


def has_ixml(path):
    return "iXML" in riff_chunk_ids(path)


def is_speaker_gated(path):
    """True if this file already carries the gated marker in its iXML NOTE."""
    try:
        with open(path, "rb") as handle:
            if handle.read(4) != b"RIFF":
                return False
            handle.seek(12)
            while True:
                header = handle.read(8)
                if len(header) < 8:
                    return False
                chunk_id = header[:4]
                size = struct.unpack("<I", header[4:8])[0]
                if chunk_id == b"iXML":
                    return GATED_NOTE in handle.read(size).decode("utf-8", "replace")
                handle.seek(size + (size & 1), 1)
    except OSError:
        return False


def write_wav_header(handle, data_size):
    """Canonical 44-byte PCM header.

    Deliberately WAVE_FORMAT_PCM with a 16-byte fmt chunk, matching what the
    server's own writers emit. WAVE_FORMAT_EXTENSIBLE -- which is what ffmpeg
    would produce for a 17-channel file -- is accepted by MonoWavStream but
    rejected by WavFileReader, which the lipsync job uses.
    """
    block_align = TARGET_CHANNELS * TARGET_BITS // 8
    byte_rate = TARGET_RATE * block_align
    handle.write(b"RIFF")
    handle.write(struct.pack("<I", 36 + data_size))
    handle.write(b"WAVE")
    handle.write(b"fmt ")
    handle.write(
        struct.pack(
            "<IHHIIHH", 16, 1, TARGET_CHANNELS, TARGET_RATE,
            byte_rate, block_align, TARGET_BITS,
        )
    )
    handle.write(b"data")
    handle.write(struct.pack("<I", data_size))


def append_ixml_chunk(path, document):
    """Append an iXML chunk and bump the outer RIFF size.

    Appending in place rather than rewriting -- these files run to hundreds of
    megabytes and the audio payload is untouched. Safe because our data chunk
    length is always even (17 channels x 2 bytes = 34 per frame), so the new
    chunk starts word-aligned.
    """
    payload = document.encode("utf-8")
    chunk = b"iXML" + struct.pack("<I", len(payload)) + payload
    if len(payload) & 1:
        chunk += b"\x00"

    with open(path, "r+b") as handle:
        handle.seek(0, os.SEEK_END)
        original_size = handle.tell()
        handle.write(chunk)
        handle.seek(4)
        handle.write(struct.pack("<I", original_size + len(chunk) - 8))
    return len(chunk)


# --- iXML generation ---------------------------------------------------------


def xml_escape(text):
    """Escape the five XML metacharacters; drop XML-1.0-illegal C0 controls.

    Ampersand first, so the entities introduced by the others aren't
    double-escaped. Mirrors xmlEscape() in IxmlWriter.cpp.
    """
    out = []
    for char in text:
        if char == "&":
            out.append("&amp;")
        elif char == "<":
            out.append("&lt;")
        elif char == ">":
            out.append("&gt;")
        elif char == '"':
            out.append("&quot;")
        elif char == "'":
            out.append("&apos;")
        elif ord(char) < 0x20 and char not in "\t\n\r":
            out.append(" ")
        else:
            out.append(char)
    return "".join(out)


def pack_timed_tokens(tokens):
    """`"start end token;start end token;..."`, seconds to 3 decimals.

    The inverse of parseTimedTokens() in IxmlTimedTokens.cpp -- a ';' inside a
    token becomes ',' because ';' is the entry separator.
    """
    parts = []
    for start, end, token in tokens:
        parts.append(f"{start:.3f} {end:.3f} {str(token).replace(';', ',')}")
    return ";".join(parts)


def build_ixml(title, take, file_uid, lane_names, transcript, mouth_cues, lanes,
               note=None, lipsync_by_lane=None):
    """Build a BWFXML document in the same shape as buildIxml()."""
    lines = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        "<BWFXML>",
        "  <IXML_VERSION>2.0</IXML_VERSION>",
        "  <PROJECT>creature-server</PROJECT>",
    ]
    if title:
        lines.append(f"  <SCENE>{xml_escape(title)}</SCENE>")
    if take:
        lines.append(f"  <TAKE>{xml_escape(take)}</TAKE>")
    lines.append("  <CIRCLED>TRUE</CIRCLED>")
    if file_uid:
        lines.append(f"  <FILE_UID>{xml_escape(file_uid)}</FILE_UID>")
    lines.append(
        "  <SPEED><FILE_SAMPLE_RATE>48000</FILE_SAMPLE_RATE>"
        "<AUDIO_BIT_DEPTH>16</AUDIO_BIT_DEPTH></SPEED>"
    )
    note = note or PLAIN_NOTE
    if title:
        note += f": {title}"
    lines.append(f"  <NOTE>{xml_escape(note)}</NOTE>")

    # Complete, contiguous TRACK_LIST -- a sparse list with a mismatched
    # TRACK_COUNT is ignored by Wave Agent and DAWs.
    lines.append("  <TRACK_LIST>")
    lines.append(f"    <TRACK_COUNT>{TARGET_CHANNELS}</TRACK_COUNT>")
    for channel in range(1, TARGET_CHANNELS + 1):
        name = xml_escape(lane_names.get(channel, ""))
        lines.append(
            f"    <TRACK><CHANNEL_INDEX>{channel}</CHANNEL_INDEX>"
            f"<NAME>{name}</NAME>"
            f"<INTERLEAVE_INDEX>{channel}</INTERLEAVE_INDEX></TRACK>"
        )
    lines.append("  </TRACK_LIST>")

    lines.append("  <USER>")
    lines.append("    <SOURCE_SCRIPT_ID></SOURCE_SCRIPT_ID>")
    lines.append(f"    <TITLE>{xml_escape(title)}</TITLE>")
    lines.append("    <GENERATION_IDS></GENERATION_IDS>")
    if transcript:
        lines.append(f"    <DIALOG_SCRIPT>{xml_escape(transcript)}</DIALOG_SCRIPT>")
    if lipsync_by_lane:
        # One Rhubarb pass per creature: each lane gets its own cues.
        lines.append("    <LIPSYNC>")
        for lane, cues in sorted(lipsync_by_lane.items()):
            if not cues:
                continue
            name = xml_escape(lane_names.get(lane, ""))
            lines.append(
                f"      <TRACK><CHANNEL_INDEX>{lane}</CHANNEL_INDEX>"
                f"<NAME>{name}</NAME>"
                f"<CUES>{xml_escape(pack_timed_tokens(cues))}</CUES></TRACK>"
            )
        lines.append("    </LIPSYNC>")
    elif mouth_cues:
        # Only a single pass over the mix exists, so it describes one mouth.
        # Attach the same cue list to each lane the audio landed on -- less
        # accurate than per-creature cues, but all that's available.
        packed = xml_escape(pack_timed_tokens(mouth_cues))
        lines.append("    <LIPSYNC>")
        for lane in lanes:
            name = xml_escape(lane_names.get(lane, ""))
            lines.append(
                f"      <TRACK><CHANNEL_INDEX>{lane}</CHANNEL_INDEX>"
                f"<NAME>{name}</NAME><CUES>{packed}</CUES></TRACK>"
            )
        lines.append("    </LIPSYNC>")
    lines.append("  </USER>")
    lines.append("</BWFXML>")
    return "\n".join(lines) + "\n"


def owns_ixml(document):
    """True only if this script wrote the chunk, so it's safe to replace.

    Server-rendered WAVs carry provenance nothing here can rebuild -- the
    ElevenLabs generation ids, the Music recipe, per-word alignment, the source
    script id. Those files are off limits; the NOTE is what distinguishes them.
    """
    match = re.search(r"<NOTE>(.*?)</NOTE>", document, re.S)
    if not match:
        return False
    note = match.group(1)
    return any(note.startswith(prefix) for prefix in OWNED_NOTE_PREFIXES)


def read_ixml(path):
    """The iXML document in `path`, or None."""
    try:
        with open(path, "rb") as handle:
            if handle.read(4) != b"RIFF":
                return None
            handle.seek(12)
            while True:
                header = handle.read(8)
                if len(header) < 8:
                    return None
                chunk_id = header[:4]
                size = struct.unpack("<I", header[4:8])[0]
                if chunk_id == b"iXML":
                    return handle.read(size).decode("utf-8", "replace")
                handle.seek(size + (size & 1), 1)
    except OSError:
        return None


def replace_ixml_chunk(path, document):
    """Swap in a new iXML chunk, or append one if the file has none.

    iXML is written last, so an existing chunk can be dropped by truncating at
    its header -- no need to rewrite the (very large) audio payload.
    """
    offset = None
    with open(path, "rb") as handle:
        handle.seek(12)
        position = 12
        while True:
            header = handle.read(8)
            if len(header) < 8:
                break
            chunk_id = header[:4]
            size = struct.unpack("<I", header[4:8])[0]
            if chunk_id == b"iXML":
                offset = position
                break
            position += 8 + size + (size & 1)
            handle.seek(position)
    if offset is not None:
        with open(path, "r+b") as handle:
            handle.truncate(offset)
    return append_ixml_chunk(path, document)


def find_per_speaker_sidecars(sounds_dir, creature_names):
    """Index the `<bit>-<NN>-<creature>.json/.txt` sidecars by their bit name.

    Some bits carry one Rhubarb pass *per creature* rather than a single pass
    over the mix -- full-duration, silent where that creature isn't talking.
    That's strictly better lipsync than duplicating whole-mix cues onto every
    lane, so it's preferred wherever it exists.

    Matching is anchored on a known creature name so a date like
    `2024-06-01_...` can't be mistaken for a `-01-` speaker index.
    """
    known = {n.lower() for n in creature_names if n}
    index = {}
    for name in sorted(os.listdir(sounds_dir)):
        stem, ext = os.path.splitext(name)
        if ext.lower() not in (".json", ".txt"):
            continue
        parts = stem.rsplit("-", 2)
        if len(parts) != 3:
            continue
        prefix, index_part, creature = parts
        if not index_part.isdigit() or creature.lower() not in known:
            continue
        entry = index.setdefault(prefix, {}).setdefault(creature.lower(), {})
        path = os.path.join(sounds_dir, name)
        if ext.lower() == ".json":
            try:
                with open(path) as handle:
                    blob = json.load(handle)
                entry["cues"] = [(float(c["start"]), float(c["end"]), str(c["value"]))
                                 for c in blob.get("mouthCues") or []]
            except (OSError, ValueError, KeyError, TypeError):
                pass
        else:
            try:
                with open(path, encoding="utf-8", errors="replace") as handle:
                    text = handle.read().strip()
                if text and text not in BOILERPLATE_TRANSCRIPTS:
                    entry["text"] = text
            except OSError:
                pass
    return index


def match_per_speaker(stem, index):
    """The per-speaker set for `stem`, tolerating small filename drift.

    The sidecars aren't perfectly named -- `mfm2025-ids-are-impotant-*` is
    missing an 'r' relative to the WAV it describes -- so an exact match alone
    would silently drop that bit's lipsync.
    """
    if stem in index:
        return index[stem]
    import difflib
    close = difflib.get_close_matches(stem, list(index), n=1, cutoff=0.9)
    return index[close[0]] if close else None


def read_sidecars(sounds_dir, relative_name):
    """Pull mouth cues and transcript out of the loose files beside a WAV."""
    stem = os.path.splitext(os.path.join(sounds_dir, relative_name))[0]

    mouth_cues = []
    json_path = stem + ".json"
    if os.path.exists(json_path):
        try:
            with open(json_path) as handle:
                blob = json.load(handle)
            for cue in blob.get("mouthCues") or []:
                mouth_cues.append(
                    (float(cue["start"]), float(cue["end"]), str(cue["value"]))
                )
        except (OSError, ValueError, KeyError, TypeError):
            mouth_cues = []

    transcript = ""
    text_path = stem + ".txt"
    if os.path.exists(text_path):
        try:
            with open(text_path, encoding="utf-8", errors="replace") as handle:
                transcript = handle.read().strip()
        except OSError:
            transcript = ""
        if transcript in BOILERPLATE_TRANSCRIPTS:
            transcript = ""

    return mouth_cues, transcript


# --- Conversion --------------------------------------------------------------


def convert(source_path, dest_path, lanes, chunk_frames=4_000_000):
    """Decode `source_path` to mono 48 kHz and splay it across `lanes`.

    `lanes` are 1-based channel numbers; each is written at interleaved index
    lane-1, every other channel is silence. Streams in chunks so a long file
    never needs its full multi-hundred-MB output buffer in memory at once.
    """
    if sys.byteorder != "little":
        raise RuntimeError("this writes little-endian samples; host is big-endian")

    indices = [lane - 1 for lane in lanes]
    temp_path = dest_path + ".tmp"

    ffmpeg = subprocess.Popen(
        [
            "ffmpeg", "-v", "error",
            "-i", source_path,
            "-ac", "1",
            "-ar", str(TARGET_RATE),
            "-f", "s16le",
            "-acodec", TARGET_CODEC,
            "-",
        ],
        stdout=subprocess.PIPE,
    )

    data_size = 0
    try:
        with open(temp_path, "wb") as out:
            write_wav_header(out, 0)  # patched below, once the size is known
            read_size = chunk_frames * 2
            while True:
                raw = ffmpeg.stdout.read(read_size)
                if not raw:
                    break
                # Drop a trailing odd byte rather than misalign the sample grid.
                if len(raw) % 2:
                    raw = raw[:-1]
                mono = array.array("h")
                mono.frombytes(raw)

                # Zero-filled frame grid, then a strided assignment per lane --
                # this is the whole reason the conversion is fast without numpy.
                frame = array.array("h", bytes(TARGET_CHANNELS * len(mono) * 2))
                for index in indices:
                    frame[index::TARGET_CHANNELS] = mono

                payload = frame.tobytes()
                out.write(payload)
                data_size += len(payload)

            out.flush()
            out.seek(0)
            write_wav_header(out, data_size)

        if ffmpeg.wait() != 0:
            raise RuntimeError(f"ffmpeg failed on {source_path}")

        os.replace(temp_path, dest_path)
    except BaseException:
        if os.path.exists(temp_path):
            os.unlink(temp_path)
        raise
    finally:
        if ffmpeg.stdout:
            ffmpeg.stdout.close()
        if ffmpeg.poll() is None:
            ffmpeg.kill()

    return data_size


# --- Speaker gating ----------------------------------------------------------
#
# The legacy stereo files are a mono mix of every voice duplicated to both
# channels (cross-correlation 1.000000 at lag 0), with only a binary +/-3 dB
# level flip distinguishing them. So the audio itself carries no separable
# per-creature content -- playing it on lanes 1 and 2 means Mango's lines come
# out of Beaky and vice versa.
#
# The animation's own track data does know who speaks when: each creature's
# track carries mouth-axis bytes at its `mouth_slot`, authored per creature. A
# creature whose mouth is moving is the one talking. Verified against the +/-3 dB
# flip on mfm2025-welcome and mfm2025-nerd-stuff -- every segment agreed, with
# the silent creature's mouth axis at exactly 0.00 stdev.
#
# So: gate the mono mix onto the speaking creature's own lane. Audio in the gaps
# (breaths, music, applause) has no owner and stays on both bird lanes.

# A creature's mouth axis varies by ~80-100 stdev while speaking and is flat
# while silent, so this threshold is nowhere near either edge.
MOUTH_ACTIVE_STDEV = 5.0
MOUTH_WINDOW_FRAMES = 10   # +/- 200 ms at the usual 20 ms/frame
MOUTH_PAD_FRAMES = 5       # keep word onsets/tails from being clipped
MOUTH_BRIDGE_FRAMES = 15   # bridge pauses shorter than ~300 ms within one turn
RAMP_MS = 20               # crossfade at lane transitions, to avoid clicks


def _flag_runs(flags):
    """Contiguous [start, end) spans where `flags` is True."""
    runs = []
    start = None
    for index, flag in enumerate(flags):
        if flag and start is None:
            start = index
        elif not flag and start is not None:
            runs.append((start, index))
            start = None
    if start is not None:
        runs.append((start, len(flags)))
    return runs


def mouth_activity(frames, mouth_slot):
    """Per-frame True/False for 'this creature is speaking'."""
    import base64
    import statistics

    values = []
    for frame in frames:
        raw = base64.b64decode(frame)
        values.append(raw[mouth_slot] if mouth_slot < len(raw) else 0)

    active = []
    for index in range(len(values)):
        low = max(0, index - MOUTH_WINDOW_FRAMES)
        high = min(len(values), index + MOUTH_WINDOW_FRAMES + 1)
        window = values[low:high]
        active.append(len(window) > 1 and statistics.pstdev(window) > MOUTH_ACTIVE_STDEV)

    # Merge runs separated by a short pause (one turn, not two), then pad the
    # edges so word onsets and tails aren't clipped. Only gaps *between* two
    # active runs are bridged -- leading and trailing silence stays silent.
    merged = []
    for start, end in _flag_runs(active):
        if merged and start - merged[-1][1] <= MOUTH_BRIDGE_FRAMES:
            merged[-1] = (merged[-1][0], end)
        else:
            merged.append((start, end))

    padded = [False] * len(active)
    for start, end in merged:
        for index in range(max(0, start - MOUTH_PAD_FRAMES),
                           min(len(active), end + MOUTH_PAD_FRAMES)):
            padded[index] = True
    return padded


def lane_runs_from_tracks(detail, lane_of, mouth_of, samples_per_frame, total_samples):
    """Sample ranges each lane owns, from the animation's per-creature mouth tracks.

    Returns (runs_by_lane, unowned_runs). `unowned_runs` covers the stretches
    where nobody is speaking.
    """
    per_creature = {}
    frame_count = 0
    for track in detail.get("tracks") or []:
        creature_id = track.get("creature_id")
        lane = lane_of.get(creature_id)
        if not lane:
            continue
        frames = track.get("frames") or []
        frame_count = max(frame_count, len(frames))
        active = mouth_activity(frames, mouth_of.get(creature_id, 4))
        # Two tracks can share a lane; OR them together.
        existing = per_creature.get(lane)
        if existing is None:
            per_creature[lane] = active
        else:
            per_creature[lane] = [a or b for a, b in
                                  zip(existing, active + [False] * len(existing))]

    if not per_creature or not frame_count:
        return {}, []

    runs_by_lane = {}
    for lane, flags in per_creature.items():
        runs_by_lane[lane] = [
            (min(int(a * samples_per_frame), total_samples),
             min(int(b * samples_per_frame), total_samples))
            for a, b in _flag_runs(flags)
        ]

    # Anything no creature claims.
    owned = [False] * frame_count
    for flags in per_creature.values():
        for index, flag in enumerate(flags):
            if flag and index < frame_count:
                owned[index] = True
    unowned = [
        (min(int(a * samples_per_frame), total_samples),
         min(int(b * samples_per_frame), total_samples))
        for a, b in _flag_runs([not o for o in owned])
    ]
    return runs_by_lane, unowned


def convert_gated(source_path, dest_path, runs_by_lane, unowned_runs, fallback_lanes):
    """Decode to mono 48 kHz, then place each stretch on the lane that owns it.

    Runs are copied with a strided slice assignment (C speed); only the short
    ramp at each run edge needs per-sample work.
    """
    if sys.byteorder != "little":
        raise RuntimeError("this writes little-endian samples; host is big-endian")

    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", source_path,
         "-ac", "1", "-ar", str(TARGET_RATE), "-f", "s16le",
         "-acodec", TARGET_CODEC, "-"],
        capture_output=True,
    ).stdout
    if not raw:
        raise RuntimeError(f"ffmpeg produced no audio for {source_path}")

    mono = array.array("h")
    mono.frombytes(raw[: len(raw) // 2 * 2])
    total = len(mono)

    out = array.array("h", bytes(TARGET_CHANNELS * total * 2))
    ramp = int(TARGET_RATE * RAMP_MS / 1000)

    placements = list(runs_by_lane.items())
    placements += [(lane, unowned_runs) for lane in fallback_lanes]

    for lane, runs in placements:
        index = lane - 1
        for start, end in runs:
            start = max(0, min(start, total))
            end = max(start, min(end, total))
            if end <= start:
                continue
            out[index + start * TARGET_CHANNELS:
                index + end * TARGET_CHANNELS: TARGET_CHANNELS] = mono[start:end]
            # Fade the edges so a lane never switches on mid-waveform.
            for offset in range(min(ramp, end - start)):
                gain = offset / ramp
                position = index + (start + offset) * TARGET_CHANNELS
                out[position] = int(out[position] * gain)
                position = index + (end - 1 - offset) * TARGET_CHANNELS
                out[position] = int(out[position] * gain)

    temp_path = dest_path + ".tmp"
    try:
        with open(temp_path, "wb") as handle:
            payload = out.tobytes()
            write_wav_header(handle, len(payload))
            handle.write(payload)
        os.replace(temp_path, dest_path)
    except BaseException:
        if os.path.exists(temp_path):
            os.unlink(temp_path)
        raise
    return TARGET_CHANNELS * total * 2


def flip_runs(source_path, total_samples):
    """Speaker runs derived from the +/-3 dB level flip, for files with no animation.

    A standalone sound has no track data to gate on, but the legacy stereo
    recordings encode the speaker as a binary level flip: the talking bird's
    side sits ~3 dB above the other. Confirmed against the mouth tracks on
    mfm2025-welcome and mfm2025-nerd-stuff, where every flip agreed with the
    creature whose mouth was moving -- louder-left is Beaky (lane 1),
    louder-right is Mango (lane 2).
    """
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", source_path, "-ac", "2",
         "-ar", str(TARGET_RATE), "-f", "s16le", "-acodec", TARGET_CODEC, "-"],
        capture_output=True,
    ).stdout
    interleaved = array.array("h")
    interleaved.frombytes(raw[: len(raw) // 4 * 4])
    left = interleaved[0::2]
    right = interleaved[1::2]

    window = TARGET_RATE // 20  # 50 ms
    sides = []
    for start in range(0, len(left) - window, window):
        chunk_l = left[start:start + window]
        chunk_r = right[start:start + window]
        energy_l = sum(x * x for x in chunk_l)
        energy_r = sum(x * x for x in chunk_r)
        if max(energy_l, energy_r) / window < 250 ** 2:
            sides.append(None)          # silence: inherit
        else:
            sides.append(1 if energy_l > energy_r else 2)

    # Hold through silence, then median-smooth to kill isolated flips.
    held = []
    current = next((s for s in sides if s), 1)
    for side in sides:
        if side:
            current = side
        held.append(current)
    smoothed = []
    for index in range(len(held)):
        neighbourhood = held[max(0, index - 2):index + 3]
        smoothed.append(1 if neighbourhood.count(1) >= neighbourhood.count(2) else 2)

    runs = {1: [], 2: []}
    for lane in (1, 2):
        for start, end in _flag_runs([s == lane for s in smoothed]):
            runs[lane].append((min(start * window, total_samples),
                               min(end * window, total_samples)))
    return {lane: spans for lane, spans in runs.items() if spans}


def is_flip_gateable(path):
    """True when a stereo file is one mono mix with a clean binary level flip."""
    info = probe(path)
    if not info or info[2] != 2:
        return False
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", path, "-f", "s16le", "-acodec", TARGET_CODEC, "-"],
        capture_output=True,
    ).stdout
    interleaved = array.array("h")
    interleaved.frombytes(raw[: len(raw) // 4 * 4])
    left = interleaved[0::2]
    right = interleaved[1::2]
    window = 22050
    correlated = 0
    active = 0
    ratios = []
    for start in range(0, len(left) - window, window):
        chunk_l = left[start:start + window]
        chunk_r = right[start:start + window]
        energy_l = sum(x * x for x in chunk_l)
        energy_r = sum(x * x for x in chunk_r)
        if max(energy_l, energy_r) / window < 300 ** 2:
            continue
        active += 1
        cross = sum(x * y for x, y in zip(chunk_l, chunk_r))
        if cross / math.sqrt(energy_l * energy_r + 1) > 0.999:
            correlated += 1
        ratios.append(math.sqrt(energy_l / (energy_r + 1e-9)))
    if active < 4 or correlated / active < 0.95:
        return False
    lows = [r for r in ratios if r < 1]
    highs = [r for r in ratios if r >= 1]
    if not lows or not highs:
        return False
    low = sorted(lows)[len(lows) // 2]
    high = sorted(highs)[len(highs) // 2]
    return abs(low * high - 1.0) < 0.06 and low < 0.95


def retire(sounds_dir, backup_dir, relative_name):
    """Move a superseded original out of the sounds root, preserving it."""
    source = os.path.join(sounds_dir, relative_name)
    dest = os.path.join(backup_dir, relative_name)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    if os.path.exists(dest):
        base, ext = os.path.splitext(dest)
        counter = 1
        while os.path.exists(f"{base}.{counter}{ext}"):
            counter += 1
        dest = f"{base}.{counter}{ext}"
    shutil.move(source, dest)
    return dest


def back_up(sounds_dir, backup_dir, relative_name):
    """Copy the original aside, preserving its path relative to the sounds root.

    The destination is outside the sounds root on purpose -- see the module
    docstring on findByBasename shadowing. Sidecars go along with it so the
    original file's metadata stays with the original audio.
    """
    saved = []
    stem, _ = os.path.splitext(relative_name)
    for candidate in (relative_name, stem + ".json", stem + ".txt"):
        source = os.path.join(sounds_dir, candidate)
        if not os.path.exists(source):
            continue
        dest = os.path.join(backup_dir, candidate)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        if os.path.exists(dest):
            base, ext = os.path.splitext(dest)
            counter = 1
            while os.path.exists(f"{base}.{counter}{ext}"):
                counter += 1
            dest = f"{base}.{counter}{ext}"
        shutil.copy2(source, dest)
        saved.append(dest)
    return saved


# --- Planning ----------------------------------------------------------------


def build_plan(server, sounds_dir, only, gate=False, backup_dir=DEFAULT_BACKUP_DIR):
    creatures = get_json(f"{server}/api/v1/creature")
    creature_items = creatures["items"] if isinstance(creatures, dict) else creatures
    lane_of = {c["id"]: c.get("audio_channel") for c in creature_items}
    name_of = {c["id"]: c.get("name", "?") for c in creature_items}
    mouth_of = {c["id"]: c.get("mouth_slot", 4) for c in creature_items}
    # Every creature that owns a lane, so the TRACK_LIST names the whole rig.
    lane_names = {c.get("audio_channel"): c.get("name", "")
                  for c in creature_items if c.get("audio_channel")}
    lane_names[BGM_LANE] = "BGM"

    animations = get_json(f"{server}/api/v1/animation")

    plan = []
    for item in animations["items"]:
        sound_file = item.get("sound_file") or ""
        if not sound_file:
            continue
        if only and only not in sound_file:
            continue

        animation_id = item["animation_id"]
        source_name = RENAMES.get(sound_file, sound_file)
        source_path = os.path.join(sounds_dir, source_name)

        # An MP3 becomes a sibling .wav; a legacy WAV is upgraded under its own
        # name so only its bytes change.
        stem, ext = os.path.splitext(source_name)
        dest_name = source_name if ext.lower() == ".wav" else stem + ".wav"
        dest_path = os.path.join(sounds_dir, dest_name)

        entry = {
            "animation_id": animation_id,
            "title": item.get("title", ""),
            "sound_file": sound_file,
            "source_name": source_name,
            "source_path": source_path,
            "dest_name": dest_name,
            "dest_path": dest_path,
            "in_place": dest_name == source_name,
            "multitrack_audio": item.get("multitrack_audio", False),
            "lane_names": lane_names,
            "creature_names": [],
        }

        # Which lanes this animation's creatures occupy.
        detail_needed = True
        dest_info = probe(dest_path) if os.path.exists(dest_path) else None
        dest_ready = dest_info is not None and is_current_format(dest_info)

        if dest_ready and not os.path.exists(source_path):
            # Converted already; the legacy source is gone (or was the same file).
            pass

        if detail_needed:
            detail = get_json(f"{server}/api/v1/animation/{animation_id}")
            creature_ids = [t.get("creature_id") for t in (detail.get("tracks") or [])]
            entry["creature_names"] = [name_of.get(cid, cid) for cid in creature_ids]
            entry["creature_ids"] = creature_ids
            # Speaker gating routes each creature to its own lane, so it isn't
            # limited to the two bird lanes the way an unseparable mix is.
            pool = {lane_of.get(cid) for cid in creature_ids} - {None}
            lanes = sorted(pool if gate else pool & set(ALLOWED_LANES))
            if not lanes:
                # No bird on lane 1 or 2 -- fall back to both so the audio is at
                # least audible rather than silently landing nowhere.
                lanes = list(ALLOWED_LANES)
                entry["lane_fallback"] = True
            entry["lanes"] = lanes
            entry["lane_of"] = lane_of
            entry["mouth_of"] = mouth_of
            entry["ms_per_frame"] = detail["metadata"].get("milliseconds_per_frame", 20)

        # Gating needs the pristine original and at least two creatures to
        # separate; a single-creature animation already lands on the right lane.
        backup_path = os.path.join(backup_dir, source_name)
        entry["backup_path"] = backup_path if os.path.exists(backup_path) else None
        entry["gate"] = bool(
            gate and len(entry.get("lanes", [])) > 1 and entry["backup_path"]
        )

        cues, transcript = read_sidecars(sounds_dir, source_name)
        entry["cues"] = cues
        entry["transcript"] = transcript
        entry["has_sidecars"] = bool(cues or transcript)

        if entry["gate"] and dest_ready and is_speaker_gated(dest_path):
            entry["gate"] = False        # already split per speaker; nothing to redo

        if entry["gate"]:
            # Re-cut from the pristine original -- the file on disk is already a
            # converted 17-channel mix and re-reading it would downmix again.
            entry["source_path"] = entry["backup_path"]
            entry["info"] = probe(entry["backup_path"])
            entry["seconds"] = duration_seconds(entry["backup_path"])
            entry["projected_bytes"] = (
                int(entry["seconds"] * TARGET_RATE) * TARGET_CHANNELS * 2
            )
            entry["too_big"] = entry["projected_bytes"] > MAX_DECODED_PCM_BYTES
            entry["action"] = "convert"
            plan.append(entry)
            continue

        if dest_ready:
            # Audio is already correct. Anything left to do is metadata.
            entry["info"] = dest_info
            entry["needs_stamp"] = entry["has_sidecars"] and not has_ixml(dest_path)
            entry["needs_repoint"] = (
                sound_file != dest_name or not entry["multitrack_audio"]
            )
            if entry["needs_stamp"]:
                entry["action"] = "stamp"
            elif entry["needs_repoint"]:
                entry["action"] = "repoint"
            else:
                entry["action"] = "ok"
            plan.append(entry)
            continue

        if not os.path.exists(source_path):
            entry["action"] = "missing"
            plan.append(entry)
            continue

        info = probe(source_path)
        entry["info"] = info
        if info is None:
            entry["action"] = "unreadable"
            plan.append(entry)
            continue

        seconds = duration_seconds(source_path)
        projected = int(seconds * TARGET_RATE) * TARGET_CHANNELS * 2
        entry["seconds"] = seconds
        entry["projected_bytes"] = projected
        entry["too_big"] = projected > MAX_DECODED_PCM_BYTES
        entry["action"] = "convert"
        plan.append(entry)

    return plan


def group_by_file(entries):
    """Collapse per-animation entries into one job per physical destination file.

    A sound file can back several animations (mfm2025-intros.wav backs four), and
    those animations do not necessarily drive the same creatures. Converting once
    per animation would be wrong twice over: the later passes would read the file
    they just rewrote and downmix 17 channels back to mono, and only the last
    animation's lane choice would survive.

    So each file is handled exactly once, onto the union of the lanes every
    animation using it asks for, and then every one of those animations is
    updated.
    """
    jobs = {}
    for entry in entries:
        job = jobs.get(entry["dest_path"])
        if job is None:
            job = {
                "source_name": entry["source_name"],
                "source_path": entry["source_path"],
                "dest_name": entry["dest_name"],
                "dest_path": entry["dest_path"],
                "in_place": entry["in_place"],
                "info": entry.get("info"),
                "lanes": set(entry.get("lanes", [])),
                "cues": entry["cues"],
                "transcript": entry["transcript"],
                "lane_names": entry["lane_names"],
                "animations": [],
                "actions": set(),
            }
            jobs[entry["dest_path"]] = job
        else:
            job["lanes"].update(entry.get("lanes", []))
        job["animations"].append(entry)
        job["actions"].add(entry["action"])

    for job in jobs.values():
        job["lanes"] = sorted(job["lanes"])
        job["shared"] = len(job["animations"]) > 1
        job["too_big"] = any(e.get("too_big") for e in job["animations"])
        # A file only needs converting once; stamping is implied by conversion.
        job["convert"] = "convert" in job["actions"]
        job["stamp"] = job["convert"] or "stamp" in job["actions"]
        job["title"] = job["animations"][0]["title"]
        job["gate"] = any(e.get("gate") for e in job["animations"])
        # One physical file, possibly several animations with different casts.
        # Derive the speaker timeline from the richest one -- a two-creature
        # animation knows about turns a single-creature one cannot.
        job["gating_entry"] = max(
            job["animations"], key=lambda e: len(e.get("lanes", []))
        ) if job["gate"] else None
    return list(jobs.values())


# --- Applying ----------------------------------------------------------------


def apply_job(job, server, sounds_dir, backup_dir):
    """Bring one file up to spec, then push every animation that uses it."""
    if job["convert"]:
        # Only back up when reading the live file; a gated re-cut already reads
        # from the backup, so backing up again would just clone it.
        if job["in_place"] and not job["gate"]:
            for saved in back_up(sounds_dir, backup_dir, job["source_name"]):
                print(f"      backed up -> {saved}")

        if job["gate"]:
            entry = job["gating_entry"]
            detail = get_json(f"{server}/api/v1/animation/{entry['animation_id']}")
            samples_per_frame = TARGET_RATE * entry["ms_per_frame"] / 1000.0
            total_samples = int(duration_seconds(job["source_path"]) * TARGET_RATE)
            runs_by_lane, unowned = lane_runs_from_tracks(
                detail, entry["lane_of"], entry["mouth_of"],
                samples_per_frame, total_samples,
            )
            if not runs_by_lane:
                raise RuntimeError("no usable mouth tracks; cannot gate")
            written = convert_gated(
                job["source_path"], job["dest_path"],
                runs_by_lane, unowned, list(ALLOWED_LANES),
            )
            spoken = {
                lane: sum(e - s for s, e in runs) / TARGET_RATE
                for lane, runs in sorted(runs_by_lane.items())
            }
            gaps = sum(e - s for s, e in unowned) / TARGET_RATE
            summary = ", ".join(f"ch{lane} {secs:.1f}s" for lane, secs in spoken.items())
            print(f"      gated {job['dest_name']} ({written / 1e6:.1f} MB) "
                  f"-- {summary}; {gaps:.1f}s unowned -> ch1+ch2")
        else:
            written = convert(job["source_path"], job["dest_path"], job["lanes"])
            print(f"      converted {job['dest_name']} "
                  f"({written / 1e6:.1f} MB, lanes {job['lanes']})")

    # A gated file always gets an iXML chunk, even with no sidecars to fold in:
    # its NOTE records that the lanes were split per speaker, which is both
    # useful provenance and how a later run knows not to re-cut it.
    if job["stamp"] and (job["cues"] or job["transcript"] or job["gate"]):
        if has_ixml(job["dest_path"]):
            print("      iXML already present, not stamping again")
        else:
            document = build_ixml(
                title=job["title"],
                take=job["animations"][0]["animation_id"],
                file_uid=job["animations"][0]["animation_id"],
                lane_names=job["lane_names"],
                transcript=job["transcript"],
                mouth_cues=job["cues"],
                lanes=job["lanes"],
                note=GATED_NOTE if job["gate"] else None,
            )
            size = append_ixml_chunk(job["dest_path"], document)
            bits = []
            if job["gate"]:
                bits.append("speaker-gated")
            if job["cues"]:
                bits.append(f"{len(job['cues'])} mouth cues")
            if job["transcript"]:
                bits.append(f"{len(job['transcript'])}-char transcript")
            print(f"      stamped iXML ({size} bytes: {', '.join(bits)})")

    # Round-trip the full animation so the server fires its Animation +
    # SoundList cache invalidations to connected clients.
    for entry in job["animations"]:
        detail = get_json(f"{server}/api/v1/animation/{entry['animation_id']}")
        detail["metadata"]["sound_file"] = job["dest_name"]
        # The file genuinely is a 17-channel multitrack WAV now, which is what
        # this flag gates (it is the precondition for lipsync generation).
        detail["metadata"]["multitrack_audio"] = True
        post_json(f"{server}/api/v1/animation", detail)
        print(f"      updated animation {entry['animation_id']} ({entry['title']})")


# --- Standalone sounds (not attached to any animation) -----------------------
#
# The console can trigger any file in the sounds root, not just animation
# audio, and playback goes through the same 17-channel loader -- so a legacy
# MP3/FLAC/44.1 kHz WAV is just as unplayable there. These have no animation
# and therefore no mouth tracks to gate on, so the lane comes from the
# filename, from the level flip when the file carries one, or from the default.

AUDIO_EXTENSIONS = (".wav", ".mp3", ".flac")
# Prefer the losslessly-encoded source, and prefer upgrading a .wav in place
# over writing across an existing one.
SOURCE_PREFERENCE = (".wav", ".flac", ".mp3")
DEFAULT_STANDALONE_LANE = 1  # Beaky: these predate the beaky_/mango_ naming


def standalone_lanes(stem):
    lowered = stem.lower()
    if lowered.startswith("beaky_"):
        return [1], "name says Beaky"
    if lowered.startswith("mango_"):
        return [2], "name says Mango"
    return [DEFAULT_STANDALONE_LANE], "unattributed"


def build_standalone_plan(sounds_dir, backup_dir, only, lanes_override=None):
    """One job per stem: the best source, its lane(s), and what it supersedes.

    With `lanes_override` this instead re-cuts already-converted files onto new
    lanes, reading from the backup original -- re-reading the live file would
    downmix an already-17-channel mix a second time.
    """
    by_stem = {}
    roots = [sounds_dir, backup_dir] if lanes_override else [sounds_dir]
    for root in roots:
        if not os.path.isdir(root):
            continue
        for name in sorted(os.listdir(root)):
            path = os.path.join(root, name)
            if not os.path.isfile(path):
                continue  # top level only; dialog/ holds modern renders
            stem, ext = os.path.splitext(name)
            if ext.lower() not in AUDIO_EXTENSIONS:
                continue
            if lanes_override:
                # Exact match only. A substring would happily also catch
                # mfm2025-1997 while re-cutting 1997, silently flattening a
                # file whose speaker gating we worked out from its animation.
                if only not in (stem, name):
                    continue
            elif only and only not in name:
                continue
            # The live file wins only when we're not re-cutting; for a re-cut the
            # backup is the pristine source and must not be shadowed by it.
            bucket = by_stem.setdefault(stem, {})
            if lanes_override and root == sounds_dir and ext.lower() in bucket:
                continue
            if lanes_override and root == backup_dir:
                bucket[ext.lower()] = path
            else:
                bucket.setdefault(ext.lower(), path)

    jobs = []
    for stem, variants in sorted(by_stem.items()):
        wav = variants.get(".wav")
        if not lanes_override and wav and is_current_format(probe(wav)):
            # The audio is already right, but two things can still be outstanding:
            # a superseded MP3/FLAC cluttering the console's list with an entry
            # that can't play, and sidecar metadata that was never folded in.
            # Correct audio is not the same as a finished file.
            playable_seconds = duration_seconds(wav)
            stale = []
            for ext, path in sorted(variants.items()):
                if ext == ".wav":
                    continue
                other = duration_seconds(path)
                if playable_seconds and abs(other - playable_seconds) / playable_seconds > 0.10:
                    continue  # a different take, not a duplicate -- leave it be
                stale.append(os.path.basename(path))

            cues, transcript = read_sidecars(sounds_dir, os.path.basename(wav))
            needs_stamp = bool(cues or transcript) and not has_ixml(wav)
            if stale or needs_stamp:
                lanes, why = standalone_lanes(stem)
                jobs.append({
                    "stem": stem, "metadata_only": True,
                    "source_path": wav, "source_name": os.path.basename(wav),
                    "dest_name": stem + ".wav", "dest_path": wav,
                    "in_place": False, "from_backup": False,
                    "info": probe(wav), "seconds": playable_seconds,
                    "lanes": lanes, "why": why, "gate": False,
                    "cues": cues if needs_stamp else [],
                    "transcript": transcript if needs_stamp else "",
                    "needs_stamp": needs_stamp,
                    "divergent": [], "too_big": False,
                    "projected_bytes": 0, "retire": stale,
                })
            continue

        source_ext = next((e for e in SOURCE_PREFERENCE if e in variants), None)
        if not source_ext:
            continue
        source_path = variants[source_ext]
        info = probe(source_path)
        if info is None:
            continue

        seconds = duration_seconds(source_path)
        from_backup = os.path.dirname(source_path) == os.path.abspath(backup_dir)
        if lanes_override:
            lanes, why, gate = lanes_override, "--lanes override", False
        else:
            lanes, why = standalone_lanes(stem)
            gate = info[2] == 2 and is_flip_gateable(source_path)
            if gate:
                lanes, why = [1, 2], "speaker flip (Beaky left / Mango right)"

        # A second variant that's a materially different take would be lost when
        # its stem is claimed by the preferred source -- surface it rather than
        # silently retiring a recording that has no playable counterpart.
        divergent = []
        for ext, path in variants.items():
            if ext == source_ext:
                continue
            other = duration_seconds(path)
            if seconds and abs(other - seconds) / seconds > 0.10:
                divergent.append((os.path.basename(path), other))

        projected = int(seconds * TARGET_RATE) * TARGET_CHANNELS * 2
        cues, transcript = read_sidecars(sounds_dir, os.path.basename(source_path))
        jobs.append({
            "stem": stem,
            "source_path": source_path,
            "source_name": os.path.basename(source_path),
            "dest_name": stem + ".wav",
            "dest_path": os.path.join(sounds_dir, stem + ".wav"),
            # Re-cutting from the backup: the original is already preserved, so
            # backing it up again would just clone it.
            "in_place": source_ext == ".wav" and not from_backup,
            "from_backup": from_backup,
            "info": info,
            "seconds": seconds,
            "lanes": lanes,
            "why": why,
            "gate": gate,
            "cues": cues,
            "transcript": transcript,
            "divergent": divergent,
            "too_big": projected > MAX_DECODED_PCM_BYTES,
            "projected_bytes": projected,
            # Everything that isn't the .wav we're about to produce is superseded
            # -- except a variant that's a materially different recording, which
            # stays put rather than being filed away with no playable counterpart.
            "retire": [] if lanes_override else
                      [os.path.basename(p) for e, p in sorted(variants.items())
                       if e != ".wav"
                       and os.path.basename(p) not in {n for n, _ in divergent}],
        })
    return jobs


def apply_standalone(job, sounds_dir, backup_dir, lane_names):
    if job.get("metadata_only"):
        if job["needs_stamp"]:
            document = build_ixml(
                title=job["stem"], take="", file_uid="", lane_names=lane_names,
                transcript=job["transcript"], mouth_cues=job["cues"],
                lanes=job["lanes"],
            )
            size = append_ixml_chunk(job["dest_path"], document)
            bits = []
            if job["cues"]:
                bits.append(f"{len(job['cues'])} mouth cues")
            if job["transcript"]:
                bits.append(f"{len(job['transcript'])}-char transcript")
            print(f"      stamped iXML ({size} bytes: {', '.join(bits)})")
        for name in job["retire"]:
            if os.path.exists(os.path.join(sounds_dir, name)):
                print(f"      retired {name} -> {retire(sounds_dir, backup_dir, name)}")
        return

    if job["in_place"]:
        for saved in back_up(sounds_dir, backup_dir, job["source_name"]):
            print(f"      backed up -> {saved}")

    if job["gate"]:
        total = int(job["seconds"] * TARGET_RATE)
        runs = flip_runs(job["source_path"], total)
        written = convert_gated(job["source_path"], job["dest_path"], runs, [],
                                fallback_lanes=[])
        spoken = ", ".join(
            f"ch{lane} {sum(e - s for s, e in spans) / TARGET_RATE:.1f}s"
            for lane, spans in sorted(runs.items())
        )
        print(f"      gated {job['dest_name']} ({written / 1e6:.1f} MB) -- {spoken}")
    else:
        written = convert(job["source_path"], job["dest_path"], job["lanes"])
        print(f"      converted {job['dest_name']} "
              f"({written / 1e6:.1f} MB, lanes {job['lanes']} -- {job['why']})")

    if (job["cues"] or job["transcript"]) and not has_ixml(job["dest_path"]):
        document = build_ixml(
            title=job["stem"], take="", file_uid="", lane_names=lane_names,
            transcript=job["transcript"], mouth_cues=job["cues"], lanes=job["lanes"],
        )
        size = append_ixml_chunk(job["dest_path"], document)
        bits = []
        if job["cues"]:
            bits.append(f"{len(job['cues'])} mouth cues")
        if job["transcript"]:
            bits.append("transcript")
        print(f"      stamped iXML ({size} bytes: {', '.join(bits)})")

    for name in job["retire"]:
        if os.path.exists(os.path.join(sounds_dir, name)):
            print(f"      retired {name} -> {retire(sounds_dir, backup_dir, name)}")


def run_standalone(args, lane_names, lanes_override=None):
    jobs = build_standalone_plan(args.sounds_dir, args.backup_dir, args.only,
                                 lanes_override)
    blocked = [j for j in jobs if j["too_big"]]
    runnable = [j for j in jobs if not j["too_big"]]

    converting = [j for j in jobs if not j.get("metadata_only")]
    metadata = [j for j in jobs if j.get("metadata_only")]
    print(f"Standalone sounds needing conversion: {len(converting)}"
          f"  (plus {len(metadata)} already playable needing metadata work)\n")
    for job in jobs:
        if job.get("metadata_only"):
            todo = []
            if job["needs_stamp"]:
                bits = []
                if job["cues"]:
                    bits.append(f"{len(job['cues'])} cues")
                if job["transcript"]:
                    bits.append("transcript")
                todo.append(f"stamp iXML ({', '.join(bits)})")
            if job["retire"]:
                todo.append(f"retire {', '.join(job['retire'])}")
            print(f"   {job['dest_name']:52} already playable -- {'; '.join(todo)}")
            continue
        codec, rate, channels = job["info"]
        note = f"{codec} {rate} Hz {channels}ch {job['seconds']:.0f}s -> lanes {job['lanes']} ({job['why']})"
        if job["too_big"]:
            note += f"  [!! {job['projected_bytes'] / 1e6:.0f} MB over the 512 MB limit]"
        print(f"   {job['source_name']:52} {note}")
        if job["retire"]:
            print(f"      retires: {', '.join(job['retire'])}")
        for name, secs in job["divergent"]:
            print(f"      NOTE {name} is {secs:.0f}s vs {job['seconds']:.0f}s "
                  f"-- a different take, retired without its own playable copy")

    if blocked:
        print(f"\n{len(blocked)} file(s) blocked and skipped.")
    if not args.apply:
        print(f"\nDry run. {len(runnable)} file(s) would be converted. Re-run with --apply.")
        return

    print(f"\nConverting {len(runnable)} file(s)...\n")
    os.makedirs(args.backup_dir, exist_ok=True)
    failures = []
    for index, job in enumerate(runnable, 1):
        print(f"[{index}/{len(runnable)}] {job['source_name']}")
        try:
            apply_standalone(job, args.sounds_dir, args.backup_dir, lane_names)
        except Exception as error:
            print(f"      FAILED: {error}")
            failures.append((job["source_name"], str(error)))

    # These files belong to no animation, so nothing else would tell the console
    # its sound list changed.
    try:
        get_json(f"{args.server}/api/v1/debug/cache-invalidate/sound-list")
        print("\nBroadcast SoundList cache invalidation to clients.")
    except Exception as error:
        print(f"\nCould not broadcast SoundList invalidation: {error}")

    print(f"\nDone. {len(runnable) - len(failures)} converted, {len(failures)} failed.")
    for name, error in failures:
        print(f"   FAILED {name}: {error}")
    if failures:
        sys.exit(1)


def run_restamp(args, lane_names, name_to_lane):
    """Rebuild every animation sound's iXML from the best sidecars available.

    Audio is untouched -- this only swaps the metadata chunk, and only when the
    document it would write differs from what's already embedded, so it settles
    to a no-op.
    """
    per_speaker = find_per_speaker_sidecars(args.sounds_dir, name_to_lane)
    animations = get_json(f"{args.server}/api/v1/animation")

    seen = {}
    for item in animations["items"]:
        sound_file = item.get("sound_file") or ""
        if not sound_file or (args.only and args.only not in sound_file):
            continue
        path = os.path.join(args.sounds_dir, sound_file)
        if not os.path.exists(path) or not is_current_format(probe(path)):
            continue
        seen.setdefault(sound_file, item)

    planned = []
    for sound_file, item in sorted(seen.items()):
        path = os.path.join(args.sounds_dir, sound_file)
        stem = os.path.splitext(os.path.basename(sound_file))[0]

        # Only ever rewrite a chunk this script wrote. Server-rendered files
        # carry provenance we cannot reconstruct -- generation ids, the
        # ElevenLabs Music recipe, per-word alignment -- and replacing their
        # iXML with our thinner document would destroy it irrecoverably.
        existing = read_ixml(path)
        if existing is not None and not owns_ixml(existing):
            continue

        speakers = match_per_speaker(stem, per_speaker)
        lipsync_by_lane = {}
        texts = []
        if speakers:
            for creature, payload in sorted(speakers.items()):
                lane = name_to_lane.get(creature)
                if lane and payload.get("cues"):
                    lipsync_by_lane[lane] = payload["cues"]
                if payload.get("text"):
                    texts.append(f"{creature.title()}: {payload['text']}")

        cues, transcript = read_sidecars(args.sounds_dir, sound_file)
        if texts:
            transcript = "\n".join(texts)

        lanes = sorted(lipsync_by_lane) or list(ALLOWED_LANES)
        document = build_ixml(
            title=item.get("title", ""),
            take=item["animation_id"], file_uid=item["animation_id"],
            lane_names=lane_names, transcript=transcript,
            mouth_cues=[] if lipsync_by_lane else cues,
            lanes=lanes,
            note=GATED_NOTE if is_speaker_gated(path) else None,
            lipsync_by_lane=lipsync_by_lane or None,
        )
        if (read_ixml(path) or "") == document:
            continue
        planned.append((sound_file, path, document, lipsync_by_lane, transcript))

    print(f"Animation sounds whose iXML needs rebuilding: {len(planned)}\n")
    for sound_file, _, _, lipsync_by_lane, transcript in planned:
        detail = []
        if lipsync_by_lane:
            detail.append("per-speaker cues " + ", ".join(
                f"ch{lane}:{len(c)}" for lane, c in sorted(lipsync_by_lane.items())))
        if transcript:
            detail.append(f"{len(transcript)}-char transcript")
        print(f"   {sound_file:52} {'; '.join(detail) or 'metadata refresh'}")

    if not args.apply:
        print(f"\nDry run. {len(planned)} file(s) would be restamped. Re-run with --apply.")
        return

    print()
    for index, (sound_file, path, document, _, _) in enumerate(planned, 1):
        size = replace_ixml_chunk(path, document)
        print(f"[{index}/{len(planned)}] {sound_file} -- iXML now {size} bytes")
    if planned:
        try:
            get_json(f"{args.server}/api/v1/debug/cache-invalidate/sound-list")
            print("\nBroadcast SoundList cache invalidation to clients.")
        except Exception as error:
            print(f"\nCould not broadcast SoundList invalidation: {error}")
    print(f"\nDone. {len(planned)} restamped.")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--server", default=DEFAULT_SERVER)
    parser.add_argument("--sounds-dir", default=DEFAULT_SOUNDS_DIR)
    parser.add_argument("--backup-dir", default=DEFAULT_BACKUP_DIR)
    parser.add_argument("--only", help="only touch sound files containing this substring")
    parser.add_argument("--gate", action="store_true",
                        help="re-cut multi-creature animations from their backups, "
                             "routing each creature's lines to its own lane using the "
                             "animation's mouth tracks")
    parser.add_argument("--restamp", action="store_true",
                        help="rebuild each animation sound's iXML from the best "
                             "sidecars available (per-creature Rhubarb passes are "
                             "preferred over a single pass over the mix). Audio is "
                             "untouched.")
    parser.add_argument("--standalone", action="store_true",
                        help="convert sounds in the sounds root that no animation "
                             "references, so the console can play them directly")
    parser.add_argument("--lanes",
                        help="with --standalone, re-cut the selected files onto these "
                             "lanes (e.g. '1,2'), reading from the backup original. "
                             "Use with --only to fix a file whose creature was guessed "
                             "wrong: --standalone --only 1997 --lanes 1,2 --apply")
    parser.add_argument("--apply", action="store_true",
                        help="actually convert and update; default is a dry run")
    args = parser.parse_args()

    if not os.path.isdir(args.sounds_dir):
        sys.exit(f"sounds directory not found: {args.sounds_dir}")

    print(f"server:  {args.server}")
    print(f"sounds:  {args.sounds_dir}")
    print(f"backups: {args.backup_dir}")
    print(f"mode:    {'APPLY' if args.apply else 'dry run'}\n")

    lanes_override = None
    if args.lanes:
        if not args.standalone:
            sys.exit("--lanes only applies with --standalone")
        try:
            lanes_override = sorted({int(x) for x in args.lanes.split(",") if x.strip()})
        except ValueError:
            sys.exit(f"could not parse --lanes {args.lanes!r}; expected e.g. '1,2'")
        if not lanes_override or any(l < 1 or l > TARGET_CHANNELS for l in lanes_override):
            sys.exit(f"--lanes must be between 1 and {TARGET_CHANNELS}")
        if not args.only:
            sys.exit("--lanes needs --only, so it can't rewrite every sound at once")

    if args.standalone or args.restamp:
        creatures = get_json(f"{args.server}/api/v1/creature")
        items = creatures["items"] if isinstance(creatures, dict) else creatures
        lane_names = {c.get("audio_channel"): c.get("name", "")
                      for c in items if c.get("audio_channel")}
        lane_names[BGM_LANE] = "BGM"
        if args.restamp:
            name_to_lane = {c.get("name", "").lower(): c.get("audio_channel")
                            for c in items if c.get("audio_channel")}
            run_restamp(args, lane_names, name_to_lane)
            return
        run_standalone(args, lane_names, lanes_override)
        return

    plan = build_plan(args.server, args.sounds_dir, args.only, gate=args.gate,
                      backup_dir=args.backup_dir)

    buckets = {}
    for entry in plan:
        buckets.setdefault(entry["action"], []).append(entry)

    ok = buckets.get("ok", [])
    ok_files = sorted({e["sound_file"] for e in ok})
    print(f"Already correct, leaving untouched: {len(ok_files)} file(s) "
          f"across {len(ok)} animation(s)")
    for name in ok_files:
        print(f"   {name}")

    missing = buckets.get("missing", []) + buckets.get("unreadable", [])
    if missing:
        print(f"\nNo usable source on disk -- reported only, nothing changed: {len(missing)}")
        for entry in sorted(missing, key=lambda e: e["sound_file"]):
            print(f"   {entry['sound_file']:34}  <- {entry['title']}")

    work = (buckets.get("convert", []) + buckets.get("stamp", [])
            + buckets.get("repoint", []))
    # An animation can be "ok" itself while sharing a file that another animation
    # is rewriting -- it still needs re-posting so clients invalidate their cache.
    touched = {e["dest_path"] for e in work}
    work += [e for e in buckets.get("ok", []) if e["dest_path"] in touched]
    jobs = group_by_file(work)
    print(f"\nWork to do: {len(jobs)} file(s) backing {len(work)} animation(s)")

    blocked = [j for j in jobs if j["too_big"]]
    for job in sorted(jobs, key=lambda j: j["source_name"]):
        steps = []
        if job["convert"]:
            codec, rate, channels = job["info"]
            if job["gate"]:
                cast = ", ".join(
                    f"{n}->ch{l}" for n, l in zip(
                        job["gating_entry"]["creature_names"],
                        [job["gating_entry"]["lane_of"].get(c)
                         for c in job["gating_entry"]["creature_ids"]])
                )
                steps.append(f"re-cut from backup with speaker gating ({cast})")
            else:
                steps.append(f"convert {codec} {rate} Hz {channels}ch -> "
                             f"{TARGET_RATE} Hz {TARGET_CHANNELS}ch lanes {job['lanes']}")
        if job["stamp"] and (job["cues"] or job["transcript"]):
            bits = []
            if job["cues"]:
                bits.append(f"{len(job['cues'])} cues")
            if job["transcript"]:
                bits.append("transcript")
            steps.append(f"stamp iXML ({', '.join(bits)})")
        if not job["in_place"]:
            steps.append(f"repoint -> {job['dest_name']}")
        if job["shared"]:
            steps.append(f"shared by {len(job['animations'])}, lanes unioned")
        if job["too_big"]:
            biggest = max(e.get("projected_bytes", 0) for e in job["animations"])
            steps.append(f"!! {biggest / 1e6:.0f} MB exceeds the 512 MB load limit")
        if not steps:
            steps.append("update animation metadata")
        print(f"   {job['source_name']}")
        print(f"      {'; '.join(steps)}")

    if blocked:
        print(f"\n{len(blocked)} file(s) are blocked and will be skipped.")

    runnable = [j for j in jobs if not j["too_big"]]

    if not args.apply:
        print(f"\nDry run. {len(runnable)} file(s) would be touched. Re-run with --apply.")
        return

    print(f"\nProcessing {len(runnable)} file(s)...\n")
    os.makedirs(args.backup_dir, exist_ok=True)

    failures = []
    for index, job in enumerate(runnable, 1):
        print(f"[{index}/{len(runnable)}] {job['source_name']}")
        try:
            apply_job(job, args.server, args.sounds_dir, args.backup_dir)
        except Exception as error:  # keep going; one bad file shouldn't stop the run
            print(f"      FAILED: {error}")
            failures.append((job["source_name"], str(error)))

    print(f"\nDone. {len(runnable) - len(failures)} succeeded, {len(failures)} failed.")
    for name, error in failures:
        print(f"   FAILED {name}: {error}")
    if failures:
        sys.exit(1)


if __name__ == "__main__":
    main()
