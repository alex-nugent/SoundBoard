// KcxLink (FirmwareSpec.md §7, Appendix A): the KCX_BT_EMITTER on Serial1.
// Open the UART before the 5 V rail, parse the module's lines (POWER ON,
// CONNECT, DISCONNECT, STATUS, SCAN), poll AT+STATUS? every 60 s while
// enabled and unlinked, send AT+POWER_OFF when the speaker is disabled, run
// the pairing flow of §7.3 (AT+PAIR, 60 s; the wipe-then-pair fallback is the
// only sender of AT+DELVMLINK), pass console AT commands through. Commands
// end with CR LF: the module ignores a bare command (bench 2026-09-14; the
// validation notes said otherwise). It answers `OK+...`. Both the app
// task (every tick) and the audio task (while the rail comes up) may feed the
// parser, so the feed is guarded by a mutex.
#pragma once
#include <stdint.h>
#include <stddef.h>

class KcxLink {
 public:
  enum class Link : uint8_t { Unknown, Linked, NotLinked, Scanning };
  enum class Pair : uint8_t { None, Wiping, Searching, Connected, Timeout };

  static constexpr uint32_t PAIR_MS = 60000;   // §7.3: "searching (60 s)"
  static constexpr uint32_t LINE_GAP_MS = 100; // bytes with no newline count as a line after this silence

  void begin();                        // Serial1 at 115200 8N1 on RX 44 / TX 43, before IO13 goes HIGH
  void end();                          // Serial1.end(), TX LOW: the rail is going down
  void feed();                         // read what the module sent; safe from two tasks
  void send(const char* cmd);          // no terminator; logged
  void powerOff();                     // AT+POWER_OFF (§7.1)
  void tick(uint32_t now, bool enabled);   // the 60 s AT+STATUS? poll while enabled and not linked; the pairing clock

  // §7.3. startPair(false) sends AT+PAIR; startPair(true) sends AT+DELVMLINK, then AT+PAIR 300 ms later.
  // Searching ends as Connected when a CONNECT line arrives after the start (so a speaker that was
  // already linked does not count), or as Timeout after PAIR_MS. The app reads pair() and calls
  // pairAck() to return to None.
  bool  startPair(bool wipe, uint32_t now);     // false when the UART is closed, the module is soft-off or never said POWER ON
  void  cancelPair();
  Pair  pair() const { return pair_; }
  void  pairAck() { if (pair_ == Pair::Connected || pair_ == Pair::Timeout) pair_ = Pair::None; }
  uint32_t pairLeftMs(uint32_t now) const;      // 0 when not searching
  bool  pairWiped() const { return pairWipe_; } // the flow in progress (or just ended) started with a wipe

  bool  powerOnSeen() const { return powerOn_; }
  bool  alive() const { return powerOn_ || okSeen_; }   // a banner, or any OK+ reply (a reset leaves the rail up and the module silent, bench 2026-09-14)
  uint32_t powerOnAtMs() const { return powerOnAt_; }
  Link  link() const { return link_; }
  bool  linked() const { return link_ == Link::Linked; }
  bool  open() const { return open_; }
  bool  softOff() const { return softOff_; }
  const char* lastLine() const { return last_; }
  const char* peerName() const { return peer_; }   // from the module's "MacAdd:...,Name:..." line
  void  setCrlf(bool on) { crlf_ = on; }           // bench: CR LF after every command (the default: the module ignores bare commands, 2026-09-14)
  bool  crlf() const { return crlf_; }
  const char* linkName() const;
  const char* pairName() const;
  uint32_t connects() const { return connects_; }
  void  railDown() { powerOn_ = false; okSeen_ = false; link_ = Link::Unknown; softOff_ = false; pair_ = Pair::None; }

 private:
  void onLine(const char* line);
  char     buf_[128];
  size_t   len_ = 0;
  char     last_[64] = { 0 };
  bool     open_ = false, powerOn_ = false, softOff_ = false, pairWipe_ = false, crlf_ = true, okSeen_ = false;
  char     peer_[24] = { 0 };
  uint32_t powerOnAt_ = 0, nextStatusAt_ = 0, lastByteAt_ = 0;
  uint32_t connects_ = 0;              // CONNECT lines seen since begin()
  uint32_t pairSendAt_ = 0, pairUntil_ = 0, pairConnects_ = 0;
  Link     link_ = Link::Unknown;
  Pair     pair_ = Pair::None;
};
