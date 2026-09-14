// Key names (FirmwareSpec.md Appendix B): keyboard usages on report 1, consumer
// bits on report 2. One row per name; case-insensitive lookup.
#pragma once
#include <stdint.h>

namespace sb {

struct KeyDef {
  const char* name;
  uint8_t     usage;         // HID keyboard usage (report 1); 0 for a consumer key
  uint8_t     modifier;      // modifier byte sent with the usage
  uint8_t     consumerBit;   // report 2 bit; 0 for a keyboard key
  bool consumer() const { return consumerBit != 0; }
};

const KeyDef* findKey(const char* name);   // nullptr when the name is not in Appendix B
uint8_t       keyCount();
const KeyDef& keyAt(uint8_t i);

}  // namespace sb
