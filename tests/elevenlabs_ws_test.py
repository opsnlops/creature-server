#!/usr/bin/env python3
"""Quick ElevenLabs WebSocket streaming test."""

import asyncio
import json
import base64

import websockets

API_KEY = "8c275a537e96131c29a878f2b76a6164"
VOICE_ID = "Nggzl2QAXh3OijoXD116"
MODEL_ID = "eleven_v3"
TEXT = "Hello! This is a quick test of the streaming speech pipeline."

async def main():
    url = (
        f"wss://api.elevenlabs.io/v1/text-to-speech/{VOICE_ID}"
        f"/stream-input?model_id={MODEL_ID}"
        f"&output_format=mp3_44100_192"
        f"&sync_alignment=true"
    )

    print(f"Connecting to: {url}")

    async with websockets.connect(url) as ws:
        print("Connected!")

        # BOS
        bos = {
            "text": " ",
            "voice_settings": {"stability": 0.3, "similarity_boost": 0.72},
            "xi_api_key": API_KEY,
            "try_trigger_generation": False,
        }
        await ws.send(json.dumps(bos))
        print("Sent BOS")

        # Text
        await ws.send(json.dumps({"text": TEXT, "try_trigger_generation": True}))
        print(f"Sent text: {TEXT}")

        # EOS
        await ws.send(json.dumps({"text": ""}))
        print("Sent EOS")

        # Receive
        total_audio = 0
        alignment_chunks = 0
        frame_count = 0

        async for message in ws:
            frame_count += 1
            if isinstance(message, bytes):
                total_audio += len(message)
                print(f"  binary frame {frame_count}: {len(message)} bytes")
            else:
                data = json.loads(message)
                if data.get("isFinal"):
                    print("Got isFinal!")
                    break
                if "audio" in data and data["audio"]:
                    audio_bytes = base64.b64decode(data["audio"])
                    total_audio += len(audio_bytes)
                if "alignment" in data and data["alignment"]:
                    alignment_chunks += 1
                    chars = "".join(data["alignment"].get("chars", []))
                    print(f"  chunk {frame_count}: {len(audio_bytes)} bytes audio, alignment: '{chars}'")
                else:
                    print(f"  msg {frame_count}: {json.dumps(data)[:200]}")

        print(f"\n=== Results ===")
        print(f"Frames received: {frame_count}")
        print(f"Total audio: {total_audio} bytes")
        print(f"Alignment chunks: {alignment_chunks}")

asyncio.run(main())
