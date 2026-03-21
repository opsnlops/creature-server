#!/usr/bin/env python3
"""
Full ElevenLabs WebSocket streaming test that mirrors exactly what
StreamingTTSClient + StreamingSpeechGenerationManager do on the server.

Tests: connect, BOS with auth, send text, receive all audio chunks +
alignment data, decode base64 audio, accumulate alignment offsets,
convert char timings to word timings, write MP3 to disk.
"""

import asyncio
import json
import base64
import time
import ssl as ssl_mod

import websockets

API_KEY = "8c275a537e96131c29a878f2b76a6164"
VOICE_ID = "Nggzl2QAXh3OijoXD116"
MODEL_ID = "eleven_turbo_v2_5"
OUTPUT_FORMAT = "mp3_44100_192"
TEXT = (
    "Now listen to this! The front door is locked, nice and tight. "
    "April is most likely attempting to train a dust bunny to do tricks, "
    "and needs concentration!"
)


async def main():
    url = (
        f"wss://api.elevenlabs.io/v1/text-to-speech/{VOICE_ID}"
        f"/stream-input?model_id={MODEL_ID}"
        f"&output_format={OUTPUT_FORMAT}"
        f"&sync_alignment=true"
    )

    print(f"URL: {url}")
    print(f"Text ({len(TEXT)} chars): {TEXT}")
    print()

    t0 = time.monotonic()

    # Connect with xi-api-key header (exactly like C++ code does)
    async with websockets.connect(
        url, additional_headers={"xi-api-key": API_KEY}
    ) as ws:
        t_connect = time.monotonic() - t0
        print(f"Connected in {t_connect*1000:.0f}ms")

        # BOS — same JSON structure as StreamingTTSClient sends
        bos = {
            "text": " ",
            "voice_settings": {"stability": 0.3, "similarity_boost": 0.72},
            "xi_api_key": API_KEY,
            "try_trigger_generation": False,
        }
        await ws.send(json.dumps(bos))

        # Text
        await ws.send(json.dumps({"text": TEXT, "try_trigger_generation": True}))

        # EOS
        await ws.send(json.dumps({"text": ""}))

        t_sent = time.monotonic() - t0
        print(f"All messages sent at {t_sent*1000:.0f}ms")
        print()

        # Receive — mirror StreamingTTSClient::receiveAllFrames exactly
        audio_data = bytearray()
        char_timings = []  # list of (char, start_ms_absolute, duration_ms)
        alignment_offset_ms = 0.0
        chunk_count = 0
        t_first_audio = None

        async for message in ws:
            if isinstance(message, bytes):
                # Binary frame — not expected but handle it
                audio_data.extend(message)
                print(f"  binary frame: {len(message)} bytes")
                continue

            data = json.loads(message)

            # Check for isFinal
            if data.get("isFinal"):
                print(f"  isFinal received")
                break

            # Decode base64 audio
            audio_b64 = data.get("audio")
            if audio_b64 and isinstance(audio_b64, str) and len(audio_b64) > 0:
                decoded = base64.b64decode(audio_b64)
                audio_data.extend(decoded)
                chunk_count += 1

                if t_first_audio is None:
                    t_first_audio = time.monotonic() - t0

            # Parse alignment — times are RELATIVE to this chunk
            alignment = data.get("alignment")
            if alignment and isinstance(alignment, dict):
                chars = alignment.get("chars", [])
                starts = alignment.get("charStartTimesMs", [])
                durations = alignment.get("charDurationsMs", [])

                count = min(len(chars), len(starts), len(durations))
                chunk_max_end = 0.0
                chunk_text = ""

                for i in range(count):
                    abs_start = starts[i] + alignment_offset_ms
                    char_timings.append((chars[i], abs_start, durations[i]))
                    chunk_text += chars[i]

                    end = starts[i] + durations[i]
                    if end > chunk_max_end:
                        chunk_max_end = end

                alignment_offset_ms += chunk_max_end

                elapsed = (time.monotonic() - t0) * 1000
                print(
                    f"  chunk {chunk_count} @ {elapsed:.0f}ms: "
                    f"{len(decoded)} bytes audio, "
                    f"{count} chars alignment, "
                    f'text="{chunk_text}"'
                )

        t_done = time.monotonic() - t0

        # === Results ===
        print()
        print("=" * 60)
        print("RESULTS")
        print("=" * 60)
        print(f"Time to connect:     {t_connect*1000:.0f}ms")
        print(f"Time to first audio: {t_first_audio*1000:.0f}ms" if t_first_audio else "No audio!")
        print(f"Total time:          {t_done*1000:.0f}ms")
        print(f"Audio chunks:        {chunk_count}")
        print(f"Total audio bytes:   {len(audio_data)}")
        print(f"Alignment chars:     {len(char_timings)}")
        print()

        # Reconstruct aligned text
        aligned_text = "".join(c for c, _, _ in char_timings)
        print(f"Aligned text: \"{aligned_text}\"")
        print()

        # Show word-level timing (reconstruct words from chars)
        print("Word timings:")
        current_word = ""
        word_start = 0.0
        word_end = 0.0
        for char, start_ms, dur_ms in char_timings:
            if char.isalpha() or char == "'":
                if not current_word:
                    word_start = start_ms
                current_word += char
                word_end = start_ms + dur_ms
            else:
                if current_word:
                    print(f"  {word_start:8.1f}ms - {word_end:8.1f}ms: \"{current_word}\"")
                    current_word = ""
        if current_word:
            print(f"  {word_start:8.1f}ms - {word_end:8.1f}ms: \"{current_word}\"")

        # Estimate audio duration from MP3 data size
        est_duration = len(audio_data) / 24000  # rough 192kbps estimate
        print(f"\nEstimated audio duration: {est_duration:.2f}s")

        # Write MP3 to disk for verification
        with open("test_output.mp3", "wb") as f:
            f.write(audio_data)
        print(f"Audio written to test_output.mp3 ({len(audio_data)} bytes)")


asyncio.run(main())
