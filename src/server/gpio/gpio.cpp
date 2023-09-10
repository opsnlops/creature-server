
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>


#include "spdlog/spdlog.h"

#include "server/config.h"
#include "server/creature-server.h"

#include "server/namespace-stuffs.h"

#include "util/environment.h"

#include "gpio.h"


namespace creatures {

    GPIO::GPIO() {

        // Start out disabled
        enabled = false;

        int useGpio = environmentToInt(USE_GPIO_ENV, DEFAULT_USE_GPIO);

        if(!useGpio) {
            debug("Not using GPIO since {} is {}", USE_GPIO_ENV, DEFAULT_USE_GPIO);
            return;
        }

        int mem_fd;
        if ((mem_fd = open(GPIO_DEVICE, O_RDWR|O_SYNC) ) < 0) {
            error("Can't open {}", GPIO_DEVICE);
            return;
        }

        // Memory map GPIO
        gpio_map = mmap(
                NULL,             // Any address will do
                GPIO_SIZE,       // Map length
                PROT_READ|PROT_WRITE, // Enable reading & writing to mapped memory
                MAP_SHARED,       // Shared with other processes
                mem_fd,           // File to map
                GPIO_BASE         // Offset to GPIO peripheral
        );

        close(mem_fd); // We don't need the file descriptor after mmap

        if (gpio_map == MAP_FAILED) {
            error("mmap error");
            return;
        }

        // Always use volatile pointer to hardware registers
        gpio = (volatile unsigned *)gpio_map;

        // Set up the pins as output pins
        set_output(gpio,SERVER_RUNNING_GPIO_PIN);
        set_output(gpio, PLAYING_ANIMATION_GPIO_PIN);
        set_output(gpio, PLAYING_SOUND_GPIO_PIN);
        set_output(gpio, RECEIVING_STREAM_FRAMES_GPIO_PIN);
        set_output(gpio, SENDING_DMX_GPIO_PIN);
        set_output(gpio, HEARTBEAT_GPIO_PIN);

        // If we made it all the way here, we're online!
        enabled = true;

    }


    void GPIO::serverOnline(bool online) {
        if(enabled) {
            if(online) {
                turn_on(SERVER_RUNNING_GPIO_PIN);
                debug("turned on the server running light");
            }
            else {
                turn_off(SERVER_RUNNING_GPIO_PIN);
                debug("turned off the server running light");
            }
        }
    }

    void GPIO::heartbeat(bool heartbeat) {
        if(enabled) {
            if(heartbeat) {
                turn_on(HEARTBEAT_GPIO_PIN);
                debug("turned on the heartbeat light");
            }
            else {
                turn_off(HEARTBEAT_GPIO_PIN);
                debug("turned off the heartbeat light");
            }
        }
    }

    void GPIO::playingAnimation(bool playingAnimation) {
        if(enabled) {
            if(playingAnimation) {
                turn_on(PLAYING_ANIMATION_GPIO_PIN);
                debug("turned on the playing animation light");
            }
            else {
                turn_off(PLAYING_ANIMATION_GPIO_PIN);
                debug("turned off the playing animation light");
            }
        }
    }

    void GPIO::playingSound(bool playingSound) {
        if(enabled) {
            if(playingSound) {
                turn_on(PLAYING_SOUND_GPIO_PIN);
                debug("turned on the playing sound light");
            }
            else {
                turn_off(PLAYING_SOUND_GPIO_PIN);
                debug("turned off the playing sound light");
            }
        }
    }

    void GPIO::receivingStreamFrames(bool receivingStreamFrames) {
        if(enabled) {
            if(receivingStreamFrames) {
                turn_on(RECEIVING_STREAM_FRAMES_GPIO_PIN);
                debug("turned on the receiving stream frames light");
            }
            else {
                turn_off(RECEIVING_STREAM_FRAMES_GPIO_PIN);
                debug("turned off the receiving stream frames light");
            }
        }
    }

    void GPIO::sendingDMX(bool sendingDMX) {
        if(enabled) {
            if(sendingDMX) {
                turn_on(SENDING_DMX_GPIO_PIN);
                debug("turned on the sending DMX light");
            }
            else {
                turn_off(SENDING_DMX_GPIO_PIN);
                debug("turned off the sending DMX light");
            }
        }
    }

    void GPIO::set_output(volatile unsigned *gpio, int g) {
        *(gpio+((g)/10)) &= ~(7<<(((g)%10)*3));
        *(gpio+((g)/10)) |=  (1<<(((g)%10)*3));
    }

    void GPIO::turn_on(int pin) {
        GPIO_SET = 1 << pin;
    }

    void GPIO::turn_off(int pin) {
        GPIO_CLR = 1 << pin;
    }

    void GPIO::allOff() {
        turn_off(SERVER_RUNNING_GPIO_PIN);
        turn_off(SENDING_DMX_GPIO_PIN);
        turn_off(RECEIVING_STREAM_FRAMES_GPIO_PIN);
        turn_off(PLAYING_SOUND_GPIO_PIN);
        turn_off(PLAYING_ANIMATION_GPIO_PIN);
        turn_off(HEARTBEAT_GPIO_PIN);
    }

    GPIO::~GPIO() {
        enabled = false;
    }

}