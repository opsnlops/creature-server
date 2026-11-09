# Advertising the server over mDNS

Plan for issue #255. The first piece of `creature-common`; the audio output sink
and `RtcpPacket` follow in their own issues.

## Why this one first

Two libraries were on the table for pulling in from the controller: the audio
library and the mDNS library. They look like one job and are not.

The server already has a working local-audio path — `NativeAudioPlaybackService`
→ `AlsaAudioOutput`/`CoreAudioOutput`, with travel mode wired through
`NativePlaybackMode::Travel`. What it has is a *diverged fork* of the
controller's backends:

| file | controller | server | state |
|---|---|---|---|
| `AlsaAudioOutput.cpp` | 462 | 267 | diverged |
| `CoreAudioOutput.cpp` | 479 | 355 | diverged |
| `AlsaMixerControl.cpp` | 141 | 123 | diverged |
| `AudioOutput.h` | 37 | 31 | diverged |
| `RtcpPacket.cpp` | — | — | diverged (241-line diff) |

And the forks disagree on purpose, not just style: the controller's backends are
richer (device enumeration by index, ALSA channel maps for named
`SpeakerChannel`s, far better diagnostics) but hardcode 48 kHz and reject more
than 8 channels; the server's are simplified but generalized to a configurable
sample rate and arbitrary channel count, which it needs for 17-channel work.
Merging them means designing a config that carries both a channel *count* and an
optional speaker *map*. That is a real decision.

mDNS has none of that. The server advertises nothing today — zero hits for
mdns/avahi/bonjour/dns-sd anywhere in `src/`. There is no fork to reconcile and
no behavior to preserve, so it proves out `creature-common` and the FetchContent
plumbing at zero merge risk, and it delivers a capability the server genuinely
lacks.

## What moves

`creature_mdns` in the controller is three files, and only two of them are
general:

| file | lines | fate |
|---|---|---|
| `network/MdnsService.{h,cpp}` | 72 | → `creature-common` |
| `network/AvahiPublisher.{h,cpp}` | 305 | → `creature-common` |
| `network/MdnsRecords.{h,cpp}` | 96 | stays: controller-specific TXT builder |

The split is already clean in the source. `MdnsService` and `AvahiPublisher`
depend only on `fmt` and (on Linux) `avahi-client`. It is the *record builders* —
`MdnsRecords.cpp` on the controller, `RouterMdns.cpp` on the router — that call
`creatures::timesync::appendTimeSyncTxt`, so **`creature-common` does not need
`creature_timesync`**. Worth stating plainly because the server has no PTP code
in-process at all, and dragging timesync in would have been the one thing that
made this messy.

Each consumer keeps its own record builder. The server's is new:
`src/server/transport/ServerMdnsRecords.{h,cpp}`.

### The Logger comes too

`AvahiPublisher.h` includes `"logging/Logger.h"`, so the Logger interface is part
of the library's public API and has to move with it. That is the right outcome
anyway — `creatures::Logger` is a generic seam, not controller-specific, and the
audio sink will want it next — but it has a consequence worth knowing up front:
**88 files in the controller include `"logging/Logger.h"`**, and they all need
rewriting to `"creature/logging/Logger.h"`.

That is a mechanical `sed` with a large line count and no behavioral risk, and it
is confined to the controller's migration PR. The alternative — giving
`creature-common` its own parallel logging interface so the controller never has
to change — would mean two logger interfaces for one job, which is the sibling
copy this whole exercise exists to avoid.

Only `Logger.h` moves. `SpdlogLogger.h` (the controller's implementation) and
`LoggingException.h` stay where they are, and the server writes its own adapter.

### Include paths

`creature-common` exposes `include/` with everything under a `creature/` prefix:

```cpp
#include "creature/logging/Logger.h"
#include "creature/network/MdnsService.h"
```

The controller's `creature_mdns` currently does
`target_include_directories(creature_mdns PUBLIC .../src)` — it hands consumers
its entire source tree as an include root. That is fine inside one project and
bad across three; a bare `logging/` and `network/` at the include root of a shared
library is exactly the kind of thing that collides later. The server's include
root is `src/`, which today has no `network/` or `logging/`, so the collision is
latent rather than real — but it costs nothing to not have it.

## The TXT contract

Service type `_creature-server._tcp`, on the HTTP port the server actually bound
(`SERVER_PORT`, default 8000 — read with `environmentToInt` in `main.cpp`, not
re-derived, so a one-off port override still advertises truthfully).

Instance name from the host name via `mdnsInstanceName()` (which lower-cases).

**Not configurable, for now.** The plan called for an `mdnsName` override in
`/etc/creature/server.json`, and that was dropped on the way in: it would mean
touching `Configuration`, `CommandLine`, `ConfigFile` and the shipped
`server.json`, with config precedence to get right, in service of a name that is
already the useful answer on both servers. `serverMdnsName()` is one function and
the knob can come along the first time the host name is the wrong answer.

Keys follow the controller's conventions: `txtvers` first, snake_case, units in
the key name.

| key | source | example |
|---|---|---|
| `txtvers` | — | `1` |
| `version` | `Version.h` | `3.51.7` |
| `api` | fixed | `/api/v1` |
| `mode` | `getTravelMode()` | `mainstage` / `travel` |
| `audio` | `getAudioMode()` | `rtp` / `local` |
| `audio_channels` | `getSoundChannels()` | `17` |
| `audio_rate_hz` | `getSoundFrequency()` | `48000` |
| `audio_device` | `getSoundDeviceName()` | local mode only |
| `rtp_port` | `config.h` `RTP_PORT` | RTP mode only, `5004` |
| `rtcp_port` | `config.h` `RTCP_PORT` | RTP mode only, `5005` |
| `sound_library` | `getSoundFileLocation()` | `/mnt/creatures/sounds` |
| `lipsync` | `getLipSyncEngine()` | `whisper` |
| `network_interface` | `getNetworkDevice()` | E1.31 interface index |

The TXT budget is 1300 bytes (`MDNS_TXT_RECORD_TARGET_BYTES`) and this is
nowhere near it, so there is room to grow. `fitTxtRecord()` trims anything
overlong and the publisher warns if the whole record would not fit one packet.

**Deliberately left out of v1:** the universe assignments. They are genuinely
useful but they are runtime state that changes as creatures come and go, so
publishing them means calling `updateTxt()` on every change to
`creatureUniverseMap` — churn on the network for information the API already
serves. Revisit once there is a reason to.

**Candidate for later:** a `ptp` field. `docs/ptp-setup.md` says the hosts are
PTP-synced and the controllers require it for RTCP playout, but the server has no
in-process monitor. That needs `creature_timesync` in `creature-common` first.

## Wiring

`main.cpp`, guarded by `CREATURE_HAS_AVAHI` exactly as the router does it:

- Build the service record *after* the HTTP listener reports it is up, so the
  advertised port is the bound port and nothing is announced for a server that
  failed to start. `start()` already blocks until the listener is bound or
  throws, so this needs no new synchronization.
- This added `boundPort()` to the `TransportServer` interface. The port is
  resolved inside the transport from `SERVER_PORT`, and reading that env var a
  second time in `main.cpp` would be a second guess that could drift from what
  was actually bound — the accessor returns the real thing. `lifecycleMutex_`
  became `mutable` so the const accessor can read under the lock. No routes
  changed, and the frozen manifest still reports 125.
- Hold the publisher for process lifetime; `stop()` in the shutdown sequence
  alongside `webServer->shutdown()`.
- Advertising is a convenience. A publisher that cannot start logs and is
  ignored; it never takes the server down. This is the library's existing
  contract and it is the right one for a show server.

On macOS the library compiles and the publisher is simply absent, matching the
controller. No Bonjour backend for now.

## Build and packaging

- `libavahi-client-dev` into the Dockerfile's apt list. **This busts the apt
  layer and therefore Phase 1**, so the first build after it is the slow
  ~15-minute one. Once only.
- `pkg_check_modules(AVAHI_CLIENT REQUIRED IMPORTED_TARGET avahi-client)` on
  Linux, as the controller does.
- `avahi-daemon` into `CPACK_DEBIAN_PACKAGE_DEPENDS` in `cmake/Package.cmake`,
  matching the controller's package. `CPACK_DEBIAN_PACKAGE_SHLIBDEPS` is already
  `ON`, so `libavahi-client3` is picked up automatically — only the daemon needs
  listing.
- `After=avahi-daemon.service` in **both** `scripts/systemd/creature-server.service`
  and `creature-server-travel.service`.
- No D-Bus obstacle: the server installs as a native systemd unit running
  `User=root`, so it reaches `avahi-daemon` over D-Bus directly. The controller
  needed a fixed system user specifically because `DynamicUser` cannot connect to
  D-Bus; that trap does not apply here.
- Adding `lib/`-adjacent FetchContent means `CMakeLists.txt` changes, which also
  busts Phase 1 — same single rebuild, no extra cost.

## `creature-common`

New public `opsnlops/creature-common`, consumed by tag:

```cmake
FetchContent_Declare(
        creature_common
        GIT_REPOSITORY https://github.com/opsnlops/creature-common.git
        GIT_TAG         v0.1.0
)
FetchContent_MakeAvailable(creature_common)
```

Exports `creature::mdns`. Tagged rather than pinned by submodule commit so both
consumers move deliberately, and because this repo already gets opus, fmt,
spdlog, and uWebSockets this way — one mechanism, not two.

### Logging

The library takes the controller's injected `creatures::Logger` interface, which
is already written and debugged. The server supplies a ~20-line adapter
forwarding to the spdlog free functions it gets from `namespace-stuffs.h`. That
keeps the server's observability stack out of the library and avoids rewriting
the library's logging to suit one consumer.

## Order of work

1. `creature-common` repo: the two mDNS files plus `Logger.h`, CMake, `v0.1.0`.
   **Done** — builds standalone on macOS, 6 tests green, warnings scoped to our
   own targets so fetched dependencies aren't judged by our standards.
2. Server: FetchContent, Dockerfile, logger adapter — build it and prove it links
   before any records exist.
3. `ServerMdnsRecords` + tests.
4. `main.cpp` wiring, packaging, systemd units.
5. Verify on the Pi: `avahi-browse -r _creature-server._tcp`.
6. Bump `VERSION.txt`.
7. Controller: migrate to `creature-common`, delete its copies, rewrite the 88
   `logging/Logger.h` includes. Separate PR in that repo, and it can land after
   the server's.

## Verification

The record builder is pure and unit-testable — mode and audio-mode permutations,
key order, no empty values, record fits the budget. The publisher is not: it
needs a live `avahi-daemon`, so it gets verified on the Pi with `avahi-browse`
rather than mocked. Checking that the advertised port matches the bound port
after a `SERVER_PORT` override is worth doing by hand once.
