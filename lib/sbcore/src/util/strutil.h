// Bounded string helpers shared by the board firmware and the native tests
// (FirmwareSpec.md §19.7 rule 7: configuration strings are bounded char[N]).
#pragma once
#include <stddef.h>
#include <string.h>
#include <ctype.h>

namespace sb {

// strlcpy: copies at most n-1 chars and always terminates. Returns strlen(src).
inline size_t copyStr(char* dst, size_t n, const char* src) {
  if (!src) src = "";
  size_t len = strlen(src);
  if (n == 0) return len;
  size_t c = len < n - 1 ? len : n - 1;
  memcpy(dst, src, c);
  dst[c] = 0;
  return len;
}

inline bool eqNoCase(const char* a, const char* b) {
  if (!a || !b) return false;
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    a++; b++;
  }
  return *a == 0 && *b == 0;
}

// Sound file names: [A-Za-z0-9._-]{1,36}\.wav (§6.3). "" is allowed (= none).
inline bool validSoundName(const char* s) {
  if (!s || !*s) return true;
  size_t len = strlen(s);
  if (len < 5 || len > 40) return false;
  for (size_t i = 0; i < len; i++) {
    char c = s[i];
    if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) return false;
  }
  const char* ext = s + len - 4;
  return eqNoCase(ext, ".wav");
}

}  // namespace sb
