#pragma once

#include <array>
#include <cstdint>

#define DEBUG_DMX_SENDER 0
#define DEBUG_STREAM_FRAMES 0

#define DEBUG_EVENT_DMX 0

#define DEBUG_ANIMATION_PLAY 0

// #define DISABLE_PLAY_SAFETY_CHECKS      1

// How often the timer event fires
#define TICK_TIME_FRAMES 60000

// How often should we send our counters to the web socket clients?
#define SEND_COUNTERS_FRAMES 15000 // 15 * 1000 (the server runs at a 1ms frame rate)

// How many frames should we wait before starting an animation
#define ANIMATION_DELAY_FRAMES 500

// How many milliseconds per frame? (This should almost always be 1.)
#define EVENT_LOOP_PERIOD_MS 1

#define DB_URI_ENV "MONGO_URI"
#define DEFAULT_DB_URI "mongodb://10.19.63.5/?serverSelectionTimeoutMS=2000"
#define SERVER_PORT_ENV "SERVER_PORT"
#define DEFAULT_SERVER_PORT 8000
#define DB_NAME "creature_server"
#define CREATURES_COLLECTION "creatures"
#define ANIMATIONS_COLLECTION "animations"
#define PLAYLISTS_COLLECTION "playlists"
#define ADHOC_ANIMATIONS_COLLECTION "adhoc_animations"
#define FIXTURES_COLLECTION "fixtures"
#define DIALOG_SCRIPTS_COLLECTION "dialog_scripts"
#define STORYBOARDS_COLLECTION "storyboards"

#define SOUND_FILE_LOCATION_ENV "SOUND_FILE_LOCATION"
#define DEFAULT_SOUND_FILE_LOCATION "sounds/"

#define SOUND_DEVICE_NAME_ENV "SOUND_DEVICE_NAME"
#define DEFAULT_SOUND_DEVICE_NAME ""

#define DIALOG_GAIN_DB_ENV "DIALOG_GAIN_DB"
#define DEFAULT_DIALOG_GAIN_DB 0.0
#define BGM_GAIN_DB_ENV "BGM_GAIN_DB"
#define DEFAULT_BGM_GAIN_DB 0.0
#define LIMITER_CEILING_DB_ENV "LIMITER_CEILING_DB"
#define DEFAULT_LIMITER_CEILING_DB -1.0
#define OUTPUT_VOLUME_PERCENT_ENV "OUTPUT_VOLUME_PERCENT"
#define ALSA_MIXER_CARD_ENV "ALSA_MIXER_CARD"
#define DEFAULT_ALSA_MIXER_CARD "default"
#define ALSA_MIXER_ELEMENT_ENV "ALSA_MIXER_ELEMENT"
#define DEFAULT_ALSA_MIXER_ELEMENT ""

#define SOUND_FREQUENCY_ENV "SOUND_FREQUENCY"
#define DEFAULT_SOUND_FREQUENCY 48000

#define SOUND_CHANNELS_ENV "SOUND_CHANNELS"
#define DEFAULT_SOUND_CHANNELS 6

#define AUDIO_MODE_ENV "AUDIO_MODE"
#define DEFAULT_AUDIO_MODE "rtp"

// Travel mode: server + controllers all on one Pi. Forces stereo local audio
// and keeps sACN multicast on-host (TTL 0).
#define TRAVEL_MODE_ENV "TRAVEL_MODE"
#define DEFAULT_TRAVEL_MODE 0

#define NETWORK_DEVICE_NAME_ENV "NETWORK_DEVICE_NAME"
#define DEFAULT_NETWORK_DEVICE_NAME "eth0"

#define VOICE_API_KEY_ENV "VOICE_API_KEY"
#define DEFAULT_VOICE_API_KEY ""

#define RHUBARB_BINARY_PATH_ENV "RHUBARB_BINARY_PATH"
#define DEFAULT_RHUBARB_BINARY_PATH "/usr/bin/rhubarb"

#define WHISPER_MODEL_PATH_ENV "WHISPER_MODEL_PATH"
#define DEFAULT_WHISPER_MODEL_PATH "/usr/share/creature-server/data/ggml-base.en.bin"

#define CMU_DICT_PATH_ENV "CMU_DICT_PATH"
#define DEFAULT_CMU_DICT_PATH "/usr/share/creature-server/data/cmudict.dict"

// Lip sync engine selection: "whisper" or "rhubarb"
#define LIP_SYNC_ENGINE_ENV "LIP_SYNC_ENGINE"
#define DEFAULT_LIP_SYNC_ENGINE "whisper"

#define ADHOC_ANIMATION_TTL_HOURS_ENV "ADHOC_ANIMATION_TTL_HOURS"
#define DEFAULT_ADHOC_ANIMATION_TTL_HOURS 12

#define HONEYCOMB_API_KEY_ENV "HONEYCOMB_API_KEY"
#define DEFAULT_HONEYCOMB_API_KEY ""

// Event loop tracing sampling rate (0.0 to 1.0)
// This controls what percentage of normal event loop frames get traced
// Errors and exceptions are always traced regardless of this setting
#define EVENT_LOOP_TRACE_SAMPLING_ENV "EVENT_LOOP_TRACE_SAMPLING"
#define DEFAULT_EVENT_LOOP_TRACE_SAMPLING 0.0005 // 0.05% = 5 in 10000 frames

// RTP Fragmentation - useful for WiFi and networks without jumbo frame support
#define RTP_FRAGMENT_PACKETS_ENV "RTP_FRAGMENT_PACKETS"
#define DEFAULT_RTP_FRAGMENT_PACKETS 0 // Disabled by default (assume jumbo frames)

// Cooperative animation RTP audio loader. Main production servers have enough
// CPU/RAM to keep several cache-hit loads resident; cache-miss encoding still
// has its own single-flight gate because each file fans out to 17 Opus workers.
#define RTP_AUDIO_LOAD_WORKERS_ENV "RTP_AUDIO_LOAD_WORKERS"
#define DEFAULT_RTP_AUDIO_LOAD_WORKERS 4
#define RTP_AUDIO_LOAD_QUEUE_CAPACITY_ENV "RTP_AUDIO_LOAD_QUEUE_CAPACITY"
#define DEFAULT_RTP_AUDIO_LOAD_QUEUE_CAPACITY 64

#define SOUND_BUFFER_SIZE 2048 // Higher = less CPU, lower = less latency

#define STREAMING_TIMEOUT_FRAMES_ENV "STREAMING_TIMEOUT_FRAMES"
// How long after the last stream frame before we declare streaming over. Frames are 1ms,
// so this is real time: 1000 = one second. The console sends a frame every 20ms, but
// Wi-Fi jitter and client-side hiccups make gaps well past 60ms routine — the old value
// of 60 declared "streaming stopped" mid-stream on any hiccup, minting a fresh session
// UUID and restarting idle every second or two for an entire live session (issue #73).
// Streaming take-over is instant regardless; the only cost of a longer timeout is the
// creature holding its last streamed pose slightly longer before idle resumes once the
// operator actually lets go.
#define DEFAULT_STREAMING_TIMEOUT_FRAMES 1000 // 1s at 1ms frame rate

// Should we use the GPIO devices for LEDs? This only works on the Raspberry Pi,
// since Macs don't have these 😅
#define USE_GPIO_ENV "USE_GPIO"
#define DEFAULT_USE_GPIO 0
#define GPIO_DEVICE "/dev/gpiomem"

// Which pins to use
#define SERVER_RUNNING_GPIO_PIN 17
#define PLAYING_ANIMATION_GPIO_PIN 27
#define PLAYING_SOUND_GPIO_PIN 22
#define RECEIVING_STREAM_FRAMES_GPIO_PIN 23
#define SENDING_DMX_GPIO_PIN 24
#define HEARTBEAT_GPIO_PIN 25

// How often should the watchdog / status lights thread run?
#define WATCHDOG_THREAD_PERIOD_MS 250

// When a client updates the database, how long should we wait before sending a message to invalidate
// the caches?
#define CACHE_INVALIDATION_DELAY_TIME 50

// RTP (Real-time Transport Protocol) settings
//
// Using standard RTP port range for better compatibility with tools like Wireshark and VLC
//

static constexpr uint16_t RTP_PORT = 5004;                          // Standard RTP port
static constexpr uint16_t RTCP_PORT = RTP_PORT + 1;                 // Conventional RTP control port
static constexpr uint32_t RTCP_REPORT_INTERVAL_MS = 1000;           // Active-stream timing update cadence
static constexpr int RTP_SRATE = 48000;                             // "Full" Opus rate (48 kHz)
static constexpr int RTP_STREAMING_CHANNELS = 17;                   // 16 creatures + 1 BGM
static constexpr int RTP_FRAME_MS = 10;                             // 10 ms frames
static constexpr int RTP_SAMPLES = RTP_SRATE * RTP_FRAME_MS / 1000; // 480
static constexpr uint8_t RTP_PRIMING_FRAMES = 4;
static constexpr uint64_t RTP_PRIMING_DURATION_FRAMES = RTP_PRIMING_FRAMES * RTP_FRAME_MS / EVENT_LOOP_PERIOD_MS;
static constexpr int RTP_PCM_BYTES = RTP_SAMPLES * sizeof(int16_t) * RTP_STREAMING_CHANNELS;
static constexpr int RTP_STANDARD_MTU_PAYLOAD = 1452; // Standard ethernet MTU minus IP/UDP/RTP headers
static constexpr int RTP_OPUS_PAYLOAD_PT = 96;        // dynamic PT we’ll advertise
static constexpr int RTP_BITRATE = 256000;            // 256 kbps for Opus (super quality)

// One multicast group per channel: 239.19.63.[1-17]
inline constexpr std::array<const char *, RTP_STREAMING_CHANNELS> RTP_GROUPS = {
    "239.19.63.1",  "239.19.63.2",  "239.19.63.3",  "239.19.63.4",  "239.19.63.5",  "239.19.63.6",
    "239.19.63.7",  "239.19.63.8",  "239.19.63.9",  "239.19.63.10", "239.19.63.11", "239.19.63.12",
    "239.19.63.13", "239.19.63.14", "239.19.63.15", "239.19.63.16",
    "239.19.63.17" // BGM
};

// Warning check for standard MTU (this will trigger on WiFi without fragmentation)
static_assert(RTP_PCM_BYTES > RTP_STANDARD_MTU_PAYLOAD,
              "Audio chunk size requires fragmentation on standard MTU networks");
