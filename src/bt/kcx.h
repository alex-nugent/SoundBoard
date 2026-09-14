// KcxLink (FirmwareSpec.md §7, Appendix A): the KCX_BT_EMITTER on Serial1.
// Phase 3 subset: open the UART before the 5 V rail, parse its lines (POWER
// ON, CONNECT, DISCONNECT, STATUS, SCAN), send AT+POWER_OFF when the speaker
// is disabled, pass console AT commands through. Commands carry no line
// terminator (memory: kcx-bt-emitter-commands). Both the app task (every
// tick) and the audio task (while the rail comes up) may feed the parser, so
// the feed is guarded by a mutex.
#pragma once
#include <stdint.h>
#include <stddef.h>

class KcxLink {
 public:
  enum class Link : uint8_t { Unknown, Linked, NotLinked, Scanning };

  void begin();                        // Serial1 at 115200 8N1 on RX 44 / TX 43, before IO13 goes HIGH
  void end();                          // Serial1.end(), TX LOW: the rail is going down
  void feed();                         // read what the module sent; safe from two tasks
  void send(const char* cmd);          // no terminator; logged
  void powerOff();                     // AT+POWER_OFF (§7.1)
  void tick(uint32_t now, bool enabled);   // the 60 s AT+STATUS? poll while enabled and not linked

  bool  powerOnSeen() const { return powerOn_; }
  uint32_t powerOnAtMs() const { return powerOnAt_; }
  Link  link() const { return link_; }
  bool  linked() const { return link_ == Link::Linked; }
  bool  open() const { return open_; }
  bool  softOff() const { return softOff_; }
  const char* lastLine() const { return last_; }
  const char* linkName() const;
  void  railDown() { powerOn_ = false; link_ = Link::Unknown; softOff_ = false; }

 private:
  void onLine(const char* line);
  char     buf_[128];
  size_t   len_ = 0;
  char     last_[64] = { 0 };
  bool     open_ = false, powerOn_ = false, softOff_ = false;
  uint32_t powerOnAt_ = 0, nextStatusAt_ = 0;
  Link     link_ = Link::Unknown;
};
