#include "libs/ajm/atrac9_decoder.h"
#include "libs/audio.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>

namespace {

using Libs::Audio::Ajm::AjmDecAt9ConfigDataInfo;
using Libs::Audio::Ajm::AjmDecAt9ParseConfigData;

int failures = 0;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "AjmTests:%d: %s\n", __LINE__, #condition);        \
      failures++;                                                              \
    }                                                                          \
  } while (false)

struct Expected {
  uint32_t channels;
  uint32_t sample_rate;
  uint32_t frame_samples;
  uint32_t superframe_samples;
  uint32_t superframe_size;
};

void CheckParse(std::array<uint8_t, 4> config, const Expected &expected) {
  AjmDecAt9ConfigDataInfo info{};
  CHECK(AjmDecAt9ParseConfigData(config.data(), &info) == 0);
  CHECK(info.channels == expected.channels);
  CHECK(info.sample_rate == expected.sample_rate);
  CHECK(info.frame_samples_per_channel == expected.frame_samples);
  CHECK(info.superframe_samples_per_channel == expected.superframe_samples);
  CHECK(info.superframe_size == expected.superframe_size);
}

void TestAt9ParseConfigData() {
  // Ghost of Yotei's mono and stereo streams: 48 kHz, 4 frames of 256 samples per superframe.
  CheckParse({0xfe, 0x70, 0x07, 0xf0}, {1, 48000, 256, 1024, 256});
  CheckParse({0xfe, 0x72, 0x0f, 0xf0}, {2, 48000, 256, 1024, 512});

  // Its 12-channel streams have no 0xFE sync byte. The console describes them (the game divides
  // by the buffer size it computes from the description); the fields after that byte say
  // 48 kHz, two channels per block, 8 frames of 1544 bytes per superframe.
  CheckParse({0x30, 0x72, 0xc0, 0xfe}, {2, 48000, 256, 2048, 12352});

  // A set validation bit is still an error, with or without the sync byte, and so is no config.
  AjmDecAt9ConfigDataInfo info{};
  const std::array<uint8_t, 4> validation_bit = {0xfe, 0x73, 0x0f, 0xf0};
  CHECK(AjmDecAt9ParseConfigData(validation_bit.data(), &info) != 0);
  const std::array<uint8_t, 4> unsynced_validation_bit = {0x30, 0x73, 0xc0, 0xfe};
  CHECK(AjmDecAt9ParseConfigData(unsynced_validation_bit.data(), &info) != 0);
  CHECK(AjmDecAt9ParseConfigData(nullptr, &info) != 0);
  CHECK(AjmDecAt9ParseConfigData(validation_bit.data(), nullptr) != 0);
}

// Only the description is lenient: decoding a stream whose layout is a guess could produce
// noise, so the decoder still refuses a config without the sync byte.
void TestAt9DecoderKeepsSyncCheck() {
  using Libs::Audio::Ajm::AjmAt9Decoder;
  using Libs::Audio::Ajm::AjmDecAt9InitializeParameters;
  using Libs::Audio::Ajm::AjmSampleEncoding;

  const auto initialize = [](std::array<uint8_t, 4> config) {
    AjmAt9Decoder decoder(2, 48000, AjmSampleEncoding::S16, 0);
    AjmDecAt9InitializeParameters params{};
    std::copy(config.begin(), config.end(), params.config_data);
    return decoder.Initialize(&params, sizeof(params)).result;
  };
  CHECK(initialize({0xfe, 0x72, 0x0f, 0xf0}) == 0);
  CHECK(initialize({0x30, 0x72, 0xc0, 0xfe}) != 0);
}

} // namespace

int main() {
  TestAt9ParseConfigData();
  TestAt9DecoderKeepsSyncCheck();
  if (failures != 0) {
    std::fprintf(stderr, "AjmTests: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("AjmTests: ok\n");
  return 0;
}
