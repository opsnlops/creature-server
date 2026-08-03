# Native local and travel audio

The always-on workshop deployment uses RTP audio by default. In that mode the
server does not enumerate, open, or otherwise initialize an ALSA/CoreAudio
device. Controllers remain responsible for receiving their dialog RTP/Opus
stream and the shared BGM stream.

`--local-audio` explicitly selects native output. `--travel-mode` also selects
native output and represents the show rig: the server and all controllers run
on one Raspberry Pi 5, sACN remains host-local, and the server renders stereo.
The creatures registered with `audio_channel` 1 and 2 route to output channels
1 and 2. BGM lane 17 is mixed at equal level into both outputs. Dialog and BGM
gains are applied before each output reaches the limiter.

The local output device is opened once for the process. A bounded silence queue
keeps ALSA/CoreAudio warm between clips, while the existing single-worker,
last-request-wins coordinator serializes animation and `/sound/play` requests.
MP3 and FLAC decoding is in-process through miniaudio. Signed 16-bit PCM WAV
files use the streaming, bounded-memory lane-aware mixer in every local mode,
so dialog and BGM gain remain independent outside travel mode as well.

## Configuration

Use `--list-sound-devices` to discover exact stable device names. Numeric audio
device selection is intentionally unsupported.

| Command line | Environment | Meaning |
|---|---|---|
| `--local-audio` / `--rtp-audio` | `AUDIO_MODE=local\|rtp` | Select transport; RTP is the default |
| `--audio-device-name NAME` | `SOUND_DEVICE_NAME` | Exact ALSA/CoreAudio device name; omitted means platform default |
| `--dialog-gain-db DB` | `DIALOG_GAIN_DB` | Dialog software gain, -90 to +12 dB |
| `--bgm-gain-db DB` | `BGM_GAIN_DB` | BGM software gain, -90 to +12 dB |
| `--limiter-ceiling-db DB` | `LIMITER_CEILING_DB` | Post-mix ceiling, -90 to 0 dB |
| `--output-volume-percent PERCENT` | `OUTPUT_VOLUME_PERCENT` | Optional ALSA hardware volume, 0–100 |
| `--alsa-mixer-card CARD` | `ALSA_MIXER_CARD` | ALSA mixer card for hardware volume |
| `--alsa-mixer-element ELEMENT` | `ALSA_MIXER_ELEMENT` | ALSA playback-volume element; empty auto-selects a usable element |

Typical travel-mode settings are:

```sh
creature-server --travel-mode \
  --audio-device-name 'plughw:CARD=S3,DEV=0' \
  --dialog-gain-db 0 \
  --bgm-gain-db -6 \
  --limiter-ceiling-db -1 \
  --output-volume-percent 75 \
  --alsa-mixer-card default \
  --alsa-mixer-element PCM
```

ALSA is the only added Linux runtime dependency for local output. SDL,
SDL_mixer, PipeWire, PulseAudio, and FFmpeg are not required by this path.
