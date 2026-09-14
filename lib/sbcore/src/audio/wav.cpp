#include "audio/wav.h"
#include <string.h>

namespace sb { namespace wav {

static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

const char* errName(Err e) {
  switch (e) {
    case Err::None: return "ok";
    case Err::TooShort: return "file too short";
    case Err::NotRiff: return "not a RIFF/WAVE file";
    case Err::NoFmt: return "no fmt chunk";
    case Err::BadFmt: return "fmt chunk too short";
    case Err::NotPcm: return "not PCM (compressed or float)";
    case Err::BadBits: return "not 16-bit";
    case Err::BadChannels: return "not mono or stereo";
    case Err::BadRate: return "sample rate outside 8-48 kHz";
    case Err::BadAlign: return "block align is not channels x 2";
    case Err::NoData: return "no data chunk";
    case Err::ZeroFrames: return "no audio frames";
    case Err::ChunkPastEnd: return "a chunk runs past the end of the file";
    case Err::Io: return "read error";
  }
  return "?";
}

Err parse(ReadFn read, void* ctx, uint32_t fileSize, Info& out) {
  out = Info();
  uint8_t h[12];
  if (fileSize < 12 + 8 + 16 + 8) return Err::TooShort;
  if (read(ctx, 0, h, 12) != 12) return Err::Io;
  if (memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) return Err::NotRiff;
  bool haveFmt = false, haveData = false;
  uint32_t pos = 12;
  while (pos + 8 <= fileSize) {
    uint8_t ch[8];
    if (read(ctx, pos, ch, 8) != 8) return Err::Io;
    uint32_t size = le32(ch + 4);
    uint32_t body = pos + 8;
    if (memcmp(ch, "fmt ", 4) == 0) {
      if (size < 16) return Err::BadFmt;
      if (body + 16 > fileSize) return Err::ChunkPastEnd;
      uint8_t f[16];
      if (read(ctx, body, f, 16) != 16) return Err::Io;
      uint16_t tag = le16(f), channels = le16(f + 2), align = le16(f + 12), bits = le16(f + 14);
      uint32_t rate = le32(f + 4);
      if (tag != 1) return Err::NotPcm;                      // 0xFFFE (extensible) is not accepted either: the converter never writes it
      if (bits != 16) return Err::BadBits;
      if (channels < 1 || channels > 2) return Err::BadChannels;
      if (rate < 8000 || rate > 48000) return Err::BadRate;
      if (align != channels * 2) return Err::BadAlign;
      out.channels = channels; out.rate = rate; out.bits = bits;
      haveFmt = true;
    } else if (memcmp(ch, "data", 4) == 0) {
      if (!haveFmt) return Err::NoFmt;                       // data before fmt: refuse rather than guess
      uint32_t avail = fileSize - body;
      if (size > avail) { size = avail; out.clamped = true; }
      out.dataOffset = body; out.dataBytes = size;
      out.frames = size / (out.channels * 2);
      haveData = true;
      break;
    } else {
      if (size > fileSize - body) return Err::ChunkPastEnd;   // LIST, fact, ... must fit
    }
    uint32_t step = size + (size & 1);                        // odd chunks are padded, fmt included
    if (step > fileSize - body) { if (haveData) break; return Err::ChunkPastEnd; }
    pos = body + step;
  }
  if (!haveFmt) return Err::NoFmt;
  if (!haveData) return Err::NoData;
  if (out.frames == 0) return Err::ZeroFrames;
  return Err::None;
}

uint32_t canonicalFrames(const Info& in) {
  if (!in.rate) return 0;
  uint64_t f = (uint64_t)in.frames * 44100u / in.rate;
  return f > 0xFFFFFFFFull ? 0 : (uint32_t)f;
}

} }  // namespace sb::wav
