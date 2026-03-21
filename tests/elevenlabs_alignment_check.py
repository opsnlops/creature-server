#!/usr/bin/env python3
"""Check if ElevenLabs alignment times are chunk-relative or absolute."""

import asyncio, json, base64, websockets

API_KEY = "8c275a537e96131c29a878f2b76a6164"
VOICE_ID = "Nggzl2QAXh3OijoXD116"

async def main():
    url = (
        f"wss://api.elevenlabs.io/v1/text-to-speech/{VOICE_ID}"
        f"/stream-input?model_id=eleven_turbo_v2_5"
        f"&output_format=mp3_44100_192&sync_alignment=true"
    )
    async with websockets.connect(url, additional_headers={"xi-api-key": API_KEY}) as ws:
        await ws.send(json.dumps({"text": " ", "voice_settings": {"stability": 0.3, "similarity_boost": 0.72}, "xi_api_key": API_KEY, "try_trigger_generation": False}))
        await ws.send(json.dumps({"text": "Hello world, this is a test.", "try_trigger_generation": True}))
        await ws.send(json.dumps({"text": ""}))

        chunk = 0
        async for msg in ws:
            data = json.loads(msg)
            if data.get("isFinal"): break
            a = data.get("alignment")
            if a:
                chunk += 1
                chars = a["chars"]
                starts = a["charStartTimesMs"]
                durs = a["charDurationsMs"]
                text = "".join(chars)
                print(f"Chunk {chunk}: text=\"{text}\"")
                print(f"  starts: {starts}")
                print(f"  durs:   {durs}")
                print(f"  range:  {starts[0]}ms - {starts[-1]+durs[-1]}ms")
                print()

asyncio.run(main())
