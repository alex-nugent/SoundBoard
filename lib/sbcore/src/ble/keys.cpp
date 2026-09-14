#include "ble/keys.h"
#include <string.h>
#include <strings.h>

namespace sb {

static const KeyDef KEYS[] = {
  {"SPACE", 0x2C, 0, 0}, {"ENTER", 0x28, 0, 0}, {"TAB", 0x2B, 0, 0}, {"ESC", 0x29, 0, 0}, {"BKSP", 0x2A, 0, 0},
  {"UP", 0x52, 0, 0}, {"DOWN", 0x51, 0, 0}, {"LEFT", 0x50, 0, 0}, {"RIGHT", 0x4F, 0, 0},
  {"1", 0x1E, 0, 0}, {"2", 0x1F, 0, 0}, {"3", 0x20, 0, 0}, {"4", 0x21, 0, 0}, {"5", 0x22, 0, 0},
  {"6", 0x23, 0, 0}, {"7", 0x24, 0, 0}, {"8", 0x25, 0, 0}, {"9", 0x26, 0, 0}, {"0", 0x27, 0, 0},
  {"A", 0x04, 0, 0}, {"B", 0x05, 0, 0}, {"C", 0x06, 0, 0}, {"D", 0x07, 0, 0}, {"E", 0x08, 0, 0}, {"F", 0x09, 0, 0},
  {"G", 0x0A, 0, 0}, {"H", 0x0B, 0, 0}, {"I", 0x0C, 0, 0}, {"J", 0x0D, 0, 0}, {"K", 0x0E, 0, 0}, {"L", 0x0F, 0, 0},
  {"M", 0x10, 0, 0}, {"N", 0x11, 0, 0}, {"O", 0x12, 0, 0}, {"P", 0x13, 0, 0}, {"Q", 0x14, 0, 0}, {"R", 0x15, 0, 0},
  {"S", 0x16, 0, 0}, {"T", 0x17, 0, 0}, {"U", 0x18, 0, 0}, {"V", 0x19, 0, 0}, {"W", 0x1A, 0, 0}, {"X", 0x1B, 0, 0},
  {"Y", 0x1C, 0, 0}, {"Z", 0x1D, 0, 0},
  {"F1", 0x3A, 0, 0}, {"F2", 0x3B, 0, 0}, {"F3", 0x3C, 0, 0}, {"F4", 0x3D, 0, 0}, {"F5", 0x3E, 0, 0}, {"F6", 0x3F, 0, 0},
  {"F7", 0x40, 0, 0}, {"F8", 0x41, 0, 0}, {"F9", 0x42, 0, 0}, {"F10", 0x43, 0, 0}, {"F11", 0x44, 0, 0}, {"F12", 0x45, 0, 0},
  {"NEXT", 0, 0, 0x01}, {"PREV", 0, 0, 0x02}, {"STOP", 0, 0, 0x04}, {"PLAY", 0, 0, 0x08},
  {"MUTE", 0, 0, 0x10}, {"VOL+", 0, 0, 0x20}, {"VOL-", 0, 0, 0x40}, {"HOME", 0, 0, 0x80},
};
static const uint8_t N_KEYS = sizeof(KEYS) / sizeof(KEYS[0]);

const KeyDef* findKey(const char* name) {
  if (!name || !*name) return nullptr;
  for (uint8_t i = 0; i < N_KEYS; i++) if (!strcasecmp(KEYS[i].name, name)) return &KEYS[i];
  return nullptr;
}
uint8_t keyCount() { return N_KEYS; }
const KeyDef& keyAt(uint8_t i) { return KEYS[i < N_KEYS ? i : 0]; }

}  // namespace sb
