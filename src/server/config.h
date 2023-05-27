
#pragma once

#define DEBUG_DMX_SENDER        0
#define DEBUG_STREAM_FRAMES     0

#define DEBUG_EVENT_DMX         0

#define DEBUG_ANIMATION_PLAY    0


#define DISABLE_PLAY_SAFETY_CHECKS  1


// How often the timer event fires
#define TICK_TIME_FRAMES        60000


#define EVENT_LOOP_PERIOD_MS    1


#define DB_URI                      "mongodb://10.3.2.11"
#define DB_NAME                     "creature_server"
#define CREATURES_COLLECTION        "creatures"
#define ANIMATIONS_COLLECTION       "animation"


#define SOUND_FILE_LOCATION_ENV     "SOUND_FILE_LOCATION"
#define DEFAULT_SOUND_FILE_LOCATION "sounds/"

// The number of the sound device to use. The devices are enumerated when the
// server starts, so use that number.
#define SOUND_DEVICE_NUMBER_ENV     "SOUND_DEVICE_NUMBER"
#define DEFAULT_SOUND_DEVICE_NUMBER 0

#define SOUND_FREQUENCY_ENV         "SOUND_FREQUENCY"
#define DEFAULT_SOUND_FREQUENCY     48000

#define SOUND_CHANNELS_ENV          "SOUND_CHANNELS"
#define DEFAULT_SOUND_CHANNELS      6

#define SOUND_BUFFER_SIZE           2048    // Higher = less CPU, lower = less latency
