// Native tests for the WAV parser and the resampler (FirmwareSpec.md §6.3, Phase 3).
#include <unity.h>
#include <string.h>
#include <vector>
#include "audio/wav.h"
#include "audio/resample.h"

using namespace sb;

static std::vector<uint8_t> file;
static size_t rd(void*, uint32_t off, void* dst, size_t n) {
  if (off >= file.size()) return 0;
  size_t k = file.size() - off < n ? file.size() - off : n;
  memcpy(dst, file.data() + off, k); return k;
}
static void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i))); }
static void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
static void tag(std::vector<uint8_t>& v, const char* s) { v.insert(v.end(), s, s + 4); }

// Builds a WAV: optional LIST chunk before fmt, fmt of `fmtSize` bytes, data of `frames` frames.
static std::vector<uint8_t> makeWav(uint16_t ch, uint32_t rate, uint16_t bits, uint32_t frames, uint16_t tagId = 1, bool list = false, uint32_t dataClaim = 0, uint16_t fmtSize = 16) {
  std::vector<uint8_t> v; tag(v, "RIFF"); put32(v, 0); tag(v, "WAVE");
  if (list) { tag(v, "LIST"); put32(v, 5); v.insert(v.end(), { 'I', 'N', 'F', 'O', 'x' }); v.push_back(0); }   // odd size, padded
  tag(v, "fmt "); put32(v, fmtSize); put16(v, tagId); put16(v, ch); put32(v, rate); put32(v, rate * ch * bits / 8); put16(v, (uint16_t)(ch * bits / 8)); put16(v, bits);
  for (uint16_t i = 16; i < fmtSize; i++) v.push_back(0);
  if (fmtSize & 1) v.push_back(0);
  tag(v, "data"); uint32_t bytes = frames * ch * bits / 8; put32(v, dataClaim ? dataClaim : bytes);
  for (uint32_t i = 0; i < bytes; i++) v.push_back((uint8_t)i);
  uint32_t riff = (uint32_t)v.size() - 8; memcpy(v.data() + 4, &riff, 4);
  return v;
}

void setUp() {} void tearDown() {}

void test_canonical_file() {
  file = makeWav(1, 44100, 16, 1000);
  wav::Info i; TEST_ASSERT_EQUAL((int)wav::Err::None, (int)wav::parse(rd, nullptr, file.size(), i));
  TEST_ASSERT_EQUAL(1, i.channels); TEST_ASSERT_EQUAL(44100, i.rate); TEST_ASSERT_EQUAL(1000, i.frames);
  TEST_ASSERT_EQUAL(44, i.dataOffset); TEST_ASSERT_FALSE(i.clamped);
  TEST_ASSERT_EQUAL(1000, wav::canonicalFrames(i));
}

void test_stereo_22k_with_list_and_padded_fmt() {
  file = makeWav(2, 22050, 16, 500, 1, true, 0, 18);
  wav::Info i; TEST_ASSERT_EQUAL((int)wav::Err::None, (int)wav::parse(rd, nullptr, file.size(), i));
  TEST_ASSERT_EQUAL(2, i.channels); TEST_ASSERT_EQUAL(22050, i.rate); TEST_ASSERT_EQUAL(500, i.frames);
  TEST_ASSERT_EQUAL(1000, wav::canonicalFrames(i));
}

void test_rejects() {
  wav::Info i;
  file = makeWav(1, 44100, 8, 100);  TEST_ASSERT_EQUAL((int)wav::Err::BadBits, (int)wav::parse(rd, nullptr, file.size(), i));
  file = makeWav(1, 44100, 16, 100, 3); TEST_ASSERT_EQUAL((int)wav::Err::NotPcm, (int)wav::parse(rd, nullptr, file.size(), i));
  file = makeWav(3, 44100, 16, 100); TEST_ASSERT_EQUAL((int)wav::Err::BadChannels, (int)wav::parse(rd, nullptr, file.size(), i));
  file = makeWav(1, 96000, 16, 100); TEST_ASSERT_EQUAL((int)wav::Err::BadRate, (int)wav::parse(rd, nullptr, file.size(), i));
  file = makeWav(1, 44100, 16, 0);   TEST_ASSERT_EQUAL((int)wav::Err::ZeroFrames, (int)wav::parse(rd, nullptr, file.size(), i));
  file = makeWav(1, 44100, 16, 100, 1, false, 0, 12); TEST_ASSERT_EQUAL((int)wav::Err::BadFmt, (int)wav::parse(rd, nullptr, file.size(), i));
  file = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E' }; TEST_ASSERT_EQUAL((int)wav::Err::TooShort, (int)wav::parse(rd, nullptr, file.size(), i));
  file = makeWav(1, 44100, 16, 100); file[0] = 'X'; TEST_ASSERT_EQUAL((int)wav::Err::NotRiff, (int)wav::parse(rd, nullptr, file.size(), i));
}

void test_data_larger_than_file_is_clamped() {
  file = makeWav(1, 44100, 16, 100, 1, false, 100000);
  wav::Info i; TEST_ASSERT_EQUAL((int)wav::Err::None, (int)wav::parse(rd, nullptr, file.size(), i));
  TEST_ASSERT_TRUE(i.clamped); TEST_ASSERT_EQUAL(100, i.frames);
}

void test_unknown_chunk_past_end_rejected() {
  file = makeWav(1, 44100, 16, 100, 1, true);
  file[16] = 0xFF; file[17] = 0xFF;                           // LIST size field (offset 16) made huge
  wav::Info i; TEST_ASSERT_EQUAL((int)wav::Err::ChunkPastEnd, (int)wav::parse(rd, nullptr, file.size(), i));
}

void test_block_resample_22k_to_44k() {
  int16_t in[100]; for (int i = 0; i < 100; i++) in[i] = (int16_t)(i * 100);
  int16_t out[200]; uint32_t n = resampleMono(in, 100, 22050, out, 200);
  TEST_ASSERT_TRUE(n >= 198 && n <= 200);
  TEST_ASSERT_EQUAL(0, out[0]); TEST_ASSERT_INT_WITHIN(2, 50, out[1]); TEST_ASSERT_INT_WITHIN(2, 100, out[2]);
}

void test_stateful_resampler_matches_block_form() {
  int16_t in[300]; for (int i = 0; i < 300; i++) in[i] = (int16_t)((i % 50) * 200 - 5000);
  int16_t ref[700]; uint32_t nref = resampleMono(in, 300, 22050, ref, 700);
  LinearResampler r; r.begin(22050);
  int16_t out[700]; uint32_t total = 0, fed = 0;
  while (fed < 300 && total < 700) {                          // feed in uneven chunks
    uint32_t chunk = fed + 37 <= 300 ? 37 : 300 - fed, used = 0;
    total += r.run(in + fed, chunk, used, out + total, 700 - total);
    if (!used && chunk) { fed += chunk; continue; }           // nothing consumed: the resampler is waiting for the next call
    fed += used;
  }
  TEST_ASSERT_TRUE(total >= nref - 4 && total <= nref);
  for (uint32_t i = 0; i < total; i++) TEST_ASSERT_INT_WITHIN(1, ref[i], out[i]);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_canonical_file);
  RUN_TEST(test_stereo_22k_with_list_and_padded_fmt);
  RUN_TEST(test_rejects);
  RUN_TEST(test_data_larger_than_file_is_clamped);
  RUN_TEST(test_unknown_chunk_past_end_rejected);
  RUN_TEST(test_block_resample_22k_to_44k);
  RUN_TEST(test_stateful_resampler_matches_block_form);
  return UNITY_END();
}
