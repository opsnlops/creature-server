
#pragma once


// Define the GPIO registers. These are from the BCM2835 manual.
#define GPIO_BASE 0x3F200000 // Raspberry Pi 2/3/4
#define GPIO_SIZE (256)

#define GPIO_SET *(gpio+7)  // sets bits which are 1, ignores bits which are 0
#define GPIO_CLR *(gpio+10) // clears bits which are 1, ignores bits which are 0
#define GPIO_IN(g) (*(gpio+13)&(1<<g)) // 0 if LOW, (1<<g) if HIGH


namespace creatures {

    class GPIO {
    public:
        GPIO();
        ~GPIO();

        void serverOnline(bool online);
        void playingAnimation(bool playingAnimation);
        void playingSound(bool playingSound);
        void receivingStreamFrames(bool receivingStreamFrames);
        void sendingDMX(bool sendingDMX);
        void heartbeat(bool heartbeat);

        /**
         * Turn off everything
         */
        void allOff();

    private:
        bool enabled;
        void *gpio_map;
        volatile unsigned *gpio;

        void turn_off(int pin);
        void turn_on(int pin);
        static void set_output(volatile unsigned *gpio, int g);

    };

}