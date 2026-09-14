// ConfigReport: the warnings and errors collected while a configuration is
// loaded or validated (FirmwareSpec.md §13.4). Shown on the console at boot
// and at the top of the portal until the next save fixes them.
#pragma once
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>

namespace sb {

struct ConfigReport {
  static constexpr uint8_t MAX_ITEMS = 24;
  struct Item { char text[100]; bool error; };

  Item    items[MAX_ITEMS];
  uint8_t count    = 0;   // items stored
  uint8_t errors   = 0;   // items that are errors (a strict validation fails)
  uint8_t overflow = 0;   // items dropped because the list was full

  void clear() { count = errors = overflow = 0; }
  bool ok() const { return errors == 0; }

  void warn(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); add(false, fmt, ap); va_end(ap);
  }
  void error(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); add(true, fmt, ap); va_end(ap);
  }
  // A structural problem: an error when validating an edit (strict), a warning
  // when loading a file at boot (the board must run with what it has).
  void structural(bool strict, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); add(strict, fmt, ap); va_end(ap);
  }

 private:
  void add(bool isError, const char* fmt, va_list ap) {
    if (isError) errors++;
    if (count >= MAX_ITEMS) { overflow++; return; }
    vsnprintf(items[count].text, sizeof items[count].text, fmt, ap);
    items[count].error = isError;
    count++;
  }
};

}  // namespace sb
