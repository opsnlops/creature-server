
#pragma once

#define DEBUG_DMX_SENDER                0
#define DEBUG_STREAM_FRAMES             0

#define DEBUG_EVENT_DMX                 0

#define DEBUG_ANIMATION_PLAY            0


//#define DISABLE_PLAY_SAFETY_CHECKS      1


// How often the timer event fires
#define TICK_TIME_FRAMES                60000

// How often should we send our counters to the web socket clients?
#define SEND_COUNTERS_FRAMES            15000         // 15 * 1000 (the server runs at a 1ms frame rate)

// How many frames should we wait before starting an animation
#define ANIMATION_DELAY_FRAMES          500

// How many milliseconds per frame? (This should almost always be 1.)
#define EVENT_LOOP_PERIOD_MS            1

#define DB_URI_ENV                      "MONGO_URI"
#define DEFAULT_DB_URI                  "mongodb://10.19.63.10/?serverSelectionTimeoutMS=2000"
#define DB_NAME                         "creature_server"
#define CREATURES_COLLECTION            "creatures"
#define ANIMATIONS_COLLECTION           "animations"
#define PLAYLISTS_COLLECTION            "playlists"


#define SOUND_FILE_LOCATION_ENV         "SOUND_FILE_LOCATION"
#define DEFAULT_SOUND_FILE_LOCATION     "sounds/"

// The number of the sound device to use. The devices are enumerated when the
// server starts, so use that number.
#define SOUND_DEVICE_NUMBER_ENV         "SOUND_DEVICE_NUMBER"
#define DEFAULT_SOUND_DEVICE_NUMBER     0

#define SOUND_FREQUENCY_ENV             "SOUND_FREQUENCY"
#define DEFAULT_SOUND_FREQUENCY         48000

#define SOUND_CHANNELS_ENV              "SOUND_CHANNELS"
#define DEFAULT_SOUND_CHANNELS          6

#define NETWORK_DEVICE_NAME_ENV         "NETWORK_DEVICE_NAME"
#define DEFAULT_NETWORK_DEVICE_NAME     "eth0"

#define VOICE_API_KEY_ENV               "VOICE_API_KEY"
#define DEFAULT_VOICE_API_KEY           ""

#define HONEYCOMB_API_KEY_ENV          "HONEYCOMB_API_KEY"
#define DEFAULT_HONEYCOMB_API_KEY      ""

#define SOUND_BUFFER_SIZE               2048    // Higher = less CPU, lower = less latency

// Should we use the GPIO devices for LEDs? This only works on the Raspberry Pi,
// since Macs don't have these 😅
#define USE_GPIO_ENV                    "USE_GPIO"
#define DEFAULT_USE_GPIO                0
#define GPIO_DEVICE                     "/dev/gpiomem"

// Which pins to use
#define SERVER_RUNNING_GPIO_PIN          17
#define PLAYING_ANIMATION_GPIO_PIN       27
#define PLAYING_SOUND_GPIO_PIN           22
#define RECEIVING_STREAM_FRAMES_GPIO_PIN 23
#define SENDING_DMX_GPIO_PIN             24
#define HEARTBEAT_GPIO_PIN               25


// How often should the watchdog / status lights thread run?
#define WATCHDOG_THREAD_PERIOD_MS        250

// When a client updates the database, how long should we wait before sending a message to invalidate
// the caches?
#define CACHE_INVALIDATION_DELAY_TIME    50
