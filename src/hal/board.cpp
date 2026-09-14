#include "hal/board.h"
#include "hal/pins.h"
#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

namespace Board {

static bool s_gatedOn = false;

void earlyPins() {
  // Rule 1 (Bug 4, brief §2.5): IO39 is MTCK with a reset pull-up; with R11
  // 100 k the motor ran whenever firmware was not driving the pin. LOW first,
  // release any deep-sleep hold, then own the pin on LEDC channel 0 (timer 0).
  pinMode(pins::MOTOR, OUTPUT); digitalWrite(pins::MOTOR, LOW);
  gpio_hold_dis((gpio_num_t)pins::MOTOR); gpio_deep_sleep_hold_dis();
  ledcAttachChannel(pins::MOTOR, pins::MOTOR_PWM_HZ, pins::PWM_BITS, pins::LEDC_CH_MOTOR);
  ledcWrite(pins::MOTOR, 0);

  // Rule 2: everything that can draw power or drive a rail is off before anything else.
  gpio_hold_dis((gpio_num_t)pins::BOOST); gpio_hold_dis((gpio_num_t)pins::LDO2);   // rule 10: held LOW through sleep, released before they are driven
  pinMode(pins::BOOST, OUTPUT);  digitalWrite(pins::BOOST, LOW);     // 5 V rail (DAC + KCX)
  pinMode(pins::AMP_EN, OUTPUT); digitalWrite(pins::AMP_EN, LOW);    // amps in shutdown
  for (int8_t p : pins::RELAY) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }   // jacks open
  // I²S pins LOW until the DAC has power (rule 5: never clock the DAC without the 5 V rail).
  for (int8_t p : { pins::I2S_BCLK, pins::I2S_LRCLK, pins::I2S_DIN }) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }
  pinMode(pins::SHIELD, INPUT);                                       // shield floats until the touch driver owns it (§4.1)
  // Buttons back to digital after an ext1 wake, then pulled up (brief §4.2).
  rtc_gpio_deinit((gpio_num_t)pins::BTN_MINUS); rtc_gpio_deinit((gpio_num_t)pins::BTN_PLUS);
  pinMode(pins::BTN_MINUS, INPUT_PULLUP); pinMode(pins::BTN_PLUS, INPUT_PULLUP);

  // Gated-rail devices: every GPIO into them LOW or input while the rail is off (rule 3, Bug 1).
  for (int8_t p : { pins::SPI_SCK, pins::SPI_MOSI, pins::SD_CS, pins::TFT_CS, pins::TFT_DC, pins::TFT_BLK }) {
    pinMode(p, OUTPUT); digitalWrite(p, LOW);
  }
  pinMode(pins::SPI_MISO, INPUT);
  pinMode(pins::LDO2, OUTPUT); digitalWrite(pins::LDO2, LOW);
  s_gatedOn = false;

  pinMode(pins::VBUS, INPUT);
  pinMode(pins::SCOPE, OUTPUT); digitalWrite(pins::SCOPE, LOW);
}

void gatedRail(bool on) {
  if (on) {
    digitalWrite(pins::LDO2, HIGH);
    delay(10);                                    // rule 3: 10 ms before the screen or SD is touched
    pinMode(pins::SD_CS, OUTPUT); digitalWrite(pins::SD_CS, HIGH);   // card deselected while the screen inits
  } else {
    digitalWrite(pins::LDO2, LOW);
  }
  s_gatedOn = on;
}

bool gatedRailOn()     { return s_gatedOn; }

void armButtonsWake() {
  for (int8_t p : { pins::BTN_MINUS, pins::BTN_PLUS }) {
    rtc_gpio_init((gpio_num_t)p);
    rtc_gpio_set_direction((gpio_num_t)p, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en((gpio_num_t)p); rtc_gpio_pulldown_dis((gpio_num_t)p);
  }
  esp_sleep_enable_ext1_wakeup_io((1ULL << pins::BTN_MINUS) | (1ULL << pins::BTN_PLUS), ESP_EXT1_WAKEUP_ANY_LOW);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);   // keeps the internal pull-ups (and the touch FSM) alive
}

void holdForSleep() {
  // The motor pin belongs to LEDC after earlyPins(): take it back as a plain
  // GPIO so the LOW is really driven before the hold latches the pad.
  ledcWrite(pins::MOTOR, 0); ledcDetach(pins::MOTOR);
  pinMode(pins::MOTOR, OUTPUT); digitalWrite(pins::MOTOR, LOW);
  digitalWrite(pins::BOOST, LOW); digitalWrite(pins::LDO2, LOW);
  gpio_hold_en((gpio_num_t)pins::MOTOR); gpio_hold_en((gpio_num_t)pins::BOOST); gpio_hold_en((gpio_num_t)pins::LDO2);
  gpio_deep_sleep_hold_en();
}

void deepSleep() { esp_deep_sleep_start(); }

uint8_t wakeButtons() {
  uint64_t m = esp_sleep_get_ext1_wakeup_status();
  uint8_t b = (uint8_t)(((m >> pins::BTN_MINUS) & 1) | (((m >> pins::BTN_PLUS) & 1) << 1));
  // The IDF start-up code clears the ext1 status before the app runs (it read
  // 0 on every button wake at CP-5), so fall back to the pins themselves: a
  // real press is still down ~30 ms after the wake.
  if (!b) b = (buttonMinusDown() ? 1 : 0) | (buttonPlusDown() ? 2 : 0);
  return b;
}
bool usbPresent()      { return digitalRead(pins::VBUS) == HIGH; }
bool buttonMinusDown() { return digitalRead(pins::BTN_MINUS) == LOW; }
bool buttonPlusDown()  { return digitalRead(pins::BTN_PLUS) == LOW; }

}  // namespace Board
