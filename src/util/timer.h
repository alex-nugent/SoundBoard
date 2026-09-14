// Timer helper (FirmwareSpec.md §2.3 rule 15, §19.7 rule 3): a deadline is due
// when (int32_t)(now - due) >= 0; never subtract a timestamp that may have
// been taken after `now`.
#pragma once
#include <stdint.h>

inline bool due(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }
