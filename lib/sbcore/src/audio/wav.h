// Bounded WAV header parser (FirmwareSpec.md §6.3). Pure: reads through a
// caller-supplied function so the native tests feed it byte arrays and the
// board feeds it a card file. Accepts 16-bit PCM, 1-2 channels, 8-48 kHz;
// rejects everything else with a named reason.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace sb { namespace wav {

struct Info {
  uint16_t channels = 0;
  uint32_t rate = 0;
  uint16_t bits = 0;
  uint32_t dataOffset = 0;    // byte offset of the first frame
  uint32_t dataBytes = 0;     // bytes of sample data (clamped to the file)
  uint32_t frames = 0;        // dataBytes / (channels * 2)
  bool     clamped = false;   // the data chunk claimed more than the file holds
};

enum class Err : uint8_t { None, TooShort, NotRiff, NoFmt, BadFmt, NotPcm, BadBits, BadChannels, BadRate, BadAlign, NoData, ZeroFrames, ChunkPastEnd, Io };

// read(ctx, offset, dst, n) returns the bytes actually read.
typedef size_t (*ReadFn)(void* ctx, uint32_t offset, void* dst, size_t n);

Err parse(ReadFn read, void* ctx, uint32_t fileSize, Info& out);
const char* errName(Err e);

// Canonical (mono, 44 100 Hz) frame count for a parsed file, with checked
// arithmetic; 0 if it would overflow.
uint32_t canonicalFrames(const Info& in);

} }  // namespace sb::wav
