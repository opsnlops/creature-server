# RTCP audio synchronization

Creature Server publishes 17 related Opus/RTP multicast streams: dialog
channels 1–16 on `239.19.63.1` through `.16`, and BGM on
`239.19.63.17`. RTP uses UDP port 5004. Each channel has an independent
SSRC, packet count, and octet count, but every channel in a frame set uses the
same 48 kHz RTP timestamp.

## Timing reports

While an RTP output generation is active, the server sends an RTCP compound
packet to each channel's multicast group on UDP port 5005. Each compound
packet contains:

- a Sender Report with the channel's current SSRC;
- the NTP wall time and corresponding shared RTP timestamp;
- the channel's cumulative RTP packet and payload-octet counts;
- an SDES CNAME shared by all 17 channels from this server.

The first report is requested after the first successful silent priming frame,
before program audio begins. Reports then repeat once per second. SSRC
rotation establishes a new clock mapping and resets the channel counters.
Failure to transmit RTCP is logged and counted, but never stops RTP audio.

The clock mapping captures wall time between two monotonic-clock reads and
uses their midpoint as the common epoch. Later reports advance wall time using
the monotonic clock. This prevents an NTP correction during a track from
creating a discontinuity. RTP arithmetic intentionally wraps at 32 bits.

## Controller contract

Controller hosts must synchronize their system clocks to the server. A
controller should retain the most recent valid Sender Report for the current
RTP SSRC and calculate the server media time for a nearby packet timestamp:

```text
rtpDelta = signedWrapDifference(packetRtp, reportRtp)
serverMediaTime = reportNtp + rtpDelta / 48000 seconds
speakerPresentationTime =
    serverMediaTime + commonPlayoutDelay + signedDeviceCompensation
```

`signedWrapDifference` must use wrap-safe 32-bit RTP arithmetic. Reports
arrive once per second, so the intended difference is always far below the
half-range ambiguity at 2^31 samples.

The controller should use BGM as the master playout timeline and decode the
dialog packet with the same RTP timestamp into that output frame. Convert the
wall-clock presentation time to a local monotonic deadline using a paired
local wall/monotonic sample. Do not repeatedly sleep against an adjustable
wall clock.

Device compensation is signed. For example, if a backend can predict that
queued samples take 8 ms to reach the DAC, it should submit or start them 8 ms
before the desired speaker presentation time. If no valid report has arrived,
the controller may temporarily use its packet-arrival fallback and switch
only at a new RTP generation to avoid a mid-track discontinuity.

RTCP aligns the desired media timeline, but it does not discipline independent
audio-device clocks. Long-running playback will additionally need slow
adaptive resampling based on the difference between the expected RTP position
and the device's measured played-sample position.

## Packet-capture verification

Capture on the server's multicast interface while starting RTP playback:

```bash
sudo tcpdump -i <interface> -s 0 -w creature-audio.pcap \
  'udp port 5004 or udp port 5005'
```

Open the capture in Wireshark and use:

```text
rtcp.sr || rtcp.sdes
```

Verify that:

1. all active multicast groups receive SR+SDES compound packets on port 5005;
2. the first SR is present during the four-packet silent priming interval;
3. all 17 reports in a report set have the same NTP and RTP timestamps;
4. every SSRC is distinct while the CNAME is identical;
5. packet and octet counts advance independently per channel;
6. a new playback generation advertises new SSRCs and reset counters;
7. the RTP timestamp continues to represent skipped late frames and wraps
   correctly at 32 bits.
