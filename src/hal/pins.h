// Pin map of the V4 PCB as built (FirmwareSpec.md §2.1, FirmwareBrief.md §2.2).
// Every hardware rule of §2.3 is applied in hal/board.cpp with a comment
// naming the bug or brief section that requires it; do not "clean them up".
#pragma once
#include <stdint.h>

namespace pins {

constexpr int8_t MOTOR      = 39;   // vibration motor (Q3/Q5/Q4). LOW is the first line of setup() (Bug 4)
constexpr int8_t BOOST      = 13;   // 5 V boost enable (DAC + KCX); off at reset
constexpr int8_t AMP_EN     = 42;   // both MAX98357 SD pins; HIGH = play
constexpr int8_t RELAY[4]   = { 40, 41, 9, 12 };   // TLP241A LEDs -> J1..J4, indexed by touch channel 2..5
constexpr int8_t I2S_BCLK   = 21;
constexpr int8_t I2S_LRCLK  = 38;
constexpr int8_t I2S_DIN    = 16;
constexpr int8_t SHIELD     = 14;   // touch shield copper (T14)
// The two buttons sit one behind the other in the case. CP-9 (Alex): "up" must be the far button and
// "down" the near one, so U5/IO8 (near) is − and U4/IO6 (far) is +; the Draft 4 pin map had them the other way.
constexpr int8_t BTN_MINUS  = 8;    // U5 (near), active low, RTC pin
constexpr int8_t BTN_PLUS   = 6;    // U4 (far), active low, RTC pin
constexpr int8_t TFT_DC     = 1;
constexpr int8_t TFT_BLK    = 7;    // LEDC channel 2 (timer 1), 5 kHz, 8-bit
constexpr int8_t TFT_CS     = 34;
constexpr int8_t TFT_RST    = -1;   // software reset: RES is off the EN net (Bug 1)
constexpr int8_t SD_CS      = 15;   // HIGH whenever the screen is addressed
constexpr int8_t SPI_SCK    = 36;
constexpr int8_t SPI_MOSI   = 35;
constexpr int8_t SPI_MISO   = 37;
constexpr int8_t LDO2       = 17;   // 3V3_GATED (screen + SD); HIGH = on
constexpr int8_t KCX_TX     = 43;   // Serial1 TX -> KCX RX
constexpr int8_t KCX_RX     = 44;   // Serial1 RX <- KCX TX
constexpr int8_t VBAT       = 10;   // ADC1 ch 9
constexpr int8_t VBUS       = 33;   // USB present
constexpr int8_t SCOPE      = 18;   // bench: HIGH at PadDown, LOW at AudioStarted (latency on a scope, CP-3); it is the ProS3 RGB LED's data pin, harmless

constexpr uint8_t TOUCH_CH[4] = { 2, 3, 4, 5 };    // touch channel n == GPIO n on the S3

constexpr uint8_t  LEDC_CH_MOTOR     = 0;   // timer 0
constexpr uint8_t  LEDC_CH_BACKLIGHT = 2;   // timer 1 (channels 2n/2n+1 share a timer; keep 0 and 2 apart)
constexpr uint32_t MOTOR_PWM_HZ      = 1000;
constexpr uint32_t BLK_PWM_HZ        = 5000;
constexpr uint8_t  PWM_BITS          = 8;

constexpr uint32_t SD_HZ  = 20000000;   // the real top clock (brief §4.4); reads validated to 25 MHz
constexpr uint32_t TFT_HZ = 32000000;

}  // namespace pins
