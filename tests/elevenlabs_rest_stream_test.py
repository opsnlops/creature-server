#!/usr/bin/env python3
"""
Test ElevenLabs REST streaming TTS with timestamps and previous_request_ids.
Mirrors what StreamingTTSClient::generateSpeechREST() does.
"""

import json
import base64
import time
import requests

API_KEY = "8c275a537e96131c29a878f2b76a6164"
VOICE_ID = "Nggzl2QAXh3OijoXD116"
MODEL_ID = "eleven_turbo_v2_5"
OUTPUT_FORMAT = "mp3_44100_192"

SENTENCES = [
    "The front door just swung wide open!",
    "I bet April left it open for a family of squirrels to move in.",
]


def generate_sentence(text, previous_request_ids=None):
    url = f"https://api.elevenlabs.io/v1/text-to-speech/{VOICE_ID}/stream/with-timestamps"

    params = {"output_format": OUTPUT_FORMAT}

    body = {
        "text": text,
        "model_id": MODEL_ID,
        "voice_settings": {
            "stability": 0.3,
            "similarity_boost": 0.72,
        },
    }

    if previous_request_ids:
        body["previous_request_ids"] = previous_request_ids

    headers = {
        "xi-api-key": API_KEY,
        "Content-Type": "application/json",
        "Accept": "text/event-stream",
    }

    print(f"\n--- TTS: \"{text}\" ---")
    if previous_request_ids:
        print(f"  previous_request_ids: {previous_request_ids}")

    t0 = time.monotonic()

    response = requests.post(url, params=params, json=body, headers=headers, stream=True)

    print(f"  Status: {response.status_code}")
    print(f"  Headers: request-id={response.headers.get('request-id', 'NOT FOUND')}")

    if response.status_code != 200:
        print(f"  ERROR: {response.text[:500]}")
        return None, None

    request_id = response.headers.get("request-id", "")

    audio_data = bytearray()
    alignment_chars = []
    chunk_count = 0

    for line in response.iter_lines():
        if not line:
            continue

        try:
            data = json.loads(line)
        except json.JSONDecodeError:
            print(f"  Non-JSON line: {line[:100]}")
            continue

        # Decode audio
        audio_b64 = data.get("audio_base64")
        if audio_b64:
            decoded = base64.b64decode(audio_b64)
            audio_data.extend(decoded)
            chunk_count += 1

        # Parse alignment
        alignment = data.get("alignment")
        if alignment:
            chars = alignment.get("characters", [])
            starts = alignment.get("character_start_times_seconds", [])
            ends = alignment.get("character_end_times_seconds", [])
            text_chunk = "".join(chars)
            alignment_chars.extend(chars)
            if chunk_count <= 3 or chunk_count % 5 == 0:
                print(f"  chunk {chunk_count}: {len(decoded) if audio_b64 else 0}B audio, "
                      f"alignment: \"{text_chunk}\"")

    elapsed = time.monotonic() - t0
    print(f"  Done: {chunk_count} chunks, {len(audio_data)} bytes, "
          f"{len(alignment_chars)} alignment chars, {elapsed*1000:.0f}ms")
    print(f"  request-id: {request_id}")

    return request_id, audio_data


def main():
    print("=" * 60)
    print("ElevenLabs REST Streaming TTS with previous_request_ids")
    print("=" * 60)

    all_audio = bytearray()
    prev_ids = []

    for i, sentence in enumerate(SENTENCES):
        request_id, audio = generate_sentence(
            sentence,
            previous_request_ids=prev_ids if prev_ids else None
        )

        if request_id and audio:
            all_audio.extend(audio)
            prev_ids = [request_id]  # Chain to next sentence
        else:
            print(f"  FAILED — skipping chain")
            prev_ids = []

    # Write combined audio
    if all_audio:
        with open("rest_stream_test.mp3", "wb") as f:
            f.write(all_audio)
        print(f"\nCombined audio: {len(all_audio)} bytes → rest_stream_test.mp3")

    # Also generate without chaining for comparison
    print("\n" + "=" * 60)
    print("Same sentences WITHOUT previous_request_ids (for comparison)")
    print("=" * 60)

    all_audio_no_chain = bytearray()
    for sentence in SENTENCES:
        _, audio = generate_sentence(sentence)
        if audio:
            all_audio_no_chain.extend(audio)

    if all_audio_no_chain:
        with open("rest_stream_test_no_chain.mp3", "wb") as f:
            f.write(all_audio_no_chain)
        print(f"\nUnchained audio: {len(all_audio_no_chain)} bytes → rest_stream_test_no_chain.mp3")


if __name__ == "__main__":
    main()
