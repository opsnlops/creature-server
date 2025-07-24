
#pragma once

// Define the GPIO registers. These are from the BCM2835 manual.
#define GPIO_BASE 0x3F200000 // Raspberry Pi 2/3/4
#define GPIO_SIZE (256)

#define GPIO_SET *(gpio + 7)                 // sets bits which are 1, ignores bits which are 0
#define GPIO_CLR *(gpio + 10)                // clears bits which are 1, ignores bits which are 0
#define GPIO_IN(g) (*(gpio + 13) & (1 << g)) // 0 if LOW, (1<<g) if HIGH

namespace creatures {

class GPIO {
  public:
    GPIO();
    ~GPIO();

    void serverOnline(bool online) const;
    void playingAnimation(bool playingAnimation) const;
    void playingSound(bool playingSound) const;
    void receivingStreamFrames(bool receivingStreamFrames) const;
    void sendingDMX(bool sendingDMX) const;
    void heartbeat(bool heartbeat) const;

    /**
     * Turn off everything
     */
    void allOff() const;

  private:
    bool enabled;
    void *gpio_map;
    volatile unsigned *gpio;

    void turn_off(int pin) const;
    void turn_on(int pin) const;
    static void set_output(volatile unsigned *gpio, int g);
};

} // namespace creatures