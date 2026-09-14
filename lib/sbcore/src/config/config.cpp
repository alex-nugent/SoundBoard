// Derived facts about a Config (FirmwareSpec.md §4.1, §5.1).
#include "config/config.h"
#include "util/strutil.h"
#include <string.h>

namespace sb {

uint8_t nSoundPads(const Config& c) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < N_PADS; i++) if (c.pads.roles[i] == Role::Sound) n++;
  return n;
}

int levelPadPos(const Config& c) {
  for (uint8_t i = 0; i < N_PADS; i++) if (c.pads.roles[i] == Role::Level) return i;
  return -1;
}

int soundIndex(const Config& c, uint8_t pos) {
  if (pos >= N_PADS || c.pads.roles[pos] != Role::Sound) return -1;
  int idx = 0;
  for (uint8_t i = 0; i < pos; i++) if (c.pads.roles[i] == Role::Sound) idx++;
  return idx;
}

void entryLabel(const Entry& e, char* out, size_t n) {
  if (e.label[0]) { copyStr(out, n, e.label); return; }
  copyStr(out, n, e.sound);
  size_t len = strlen(out);
  if (len > 4 && eqNoCase(out + len - 4, ".wav")) out[len - 4] = 0;
}

}  // namespace sb
