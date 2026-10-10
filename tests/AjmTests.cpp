#include "libs/ajm/atrac9_decoder.h"
#include "libs/audio.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

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

  // Its extended streams (first byte 0x30, PS5PCEM's layout) multiplex independent mono
  // substreams: 2 * (pairs + 1) channels of (raw[3] >> 2) + 1 bytes per frame, 1 << (raw[3] & 3)
  // frames per superframe. 30 72 c0 fe: 12 channels of 64-byte frames, 4 frames.
  CheckParse({0x30, 0x72, 0xc0, 0xfe}, {12, 48000, 256, 1024, 12 * 64 * 4});
  CheckParse({0x30, 0x71, 0xc0, 0xfe}, {8, 48000, 256, 1024, 8 * 64 * 4});
  // The ambisonic beds: first order (4 channels, 48-byte frames) and fifth order (36 channels).
  CheckParse({0x30, 0x70, 0xc0, 0xbe}, {4, 48000, 256, 1024, 4 * 48 * 4});
  CheckParse({0x30, 0x78, 0xc0, 0xfe}, {36, 48000, 256, 1024, 36 * 64 * 4});

  // With the sync byte, a set validation bit is still an error, and so is no config.
  AjmDecAt9ConfigDataInfo info{};
  const std::array<uint8_t, 4> validation_bit = {0xfe, 0x73, 0x0f, 0xf0};
  CHECK(AjmDecAt9ParseConfigData(validation_bit.data(), &info) != 0);
  CHECK(AjmDecAt9ParseConfigData(nullptr, &info) != 0);
  CHECK(AjmDecAt9ParseConfigData(validation_bit.data(), nullptr) != 0);
}

std::array<uint8_t, 4> Extended(uint32_t channels, uint32_t frame_bytes, uint32_t exponent) {
  const uint32_t pairs = channels / 2 - 1;
  return {0x30, static_cast<uint8_t>(0x70 | (pairs >> 1)),
          static_cast<uint8_t>(0x40 | ((pairs & 1) << 7)),
          static_cast<uint8_t>(((frame_bytes - 1) << 2) | exponent)};
}

int Initialize(Libs::Audio::Ajm::AjmAt9Decoder &decoder, std::array<uint8_t, 4> config) {
  Libs::Audio::Ajm::AjmDecAt9InitializeParameters params{};
  std::copy(config.begin(), config.end(), params.config_data);
  return decoder.Initialize(&params, sizeof(params)).result;
}

// The classic decoder keeps the sync check; an extended config with a wrong signature or reserved
// bits, or more than 36 channels, is refused.
void TestAt9DecoderConfigs() {
  using Libs::Audio::Ajm::AjmAt9Decoder;
  using Libs::Audio::Ajm::AjmSampleEncoding;
  const auto initialize = [](std::array<uint8_t, 4> config) {
    AjmAt9Decoder decoder(2, 48000, AjmSampleEncoding::S16, 0);
    return Initialize(decoder, config);
  };
  CHECK(initialize({0xfe, 0x72, 0x0f, 0xf0}) == 0);
  CHECK(initialize({0x30, 0x72, 0xc0, 0xfe}) == 0);
  CHECK(initialize({0x30, 0x70, 0xc0, 0xbe}) == 0);
  auto signature = Extended(12, 64, 2);
  signature[0] = 0x31;
  auto reserved = Extended(12, 64, 2);
  reserved[2] |= 1;
  CHECK(initialize(signature) != 0);
  CHECK(initialize(reserved) != 0);
  CHECK(initialize(Extended(38, 64, 2)) != 0);
}

// An extended stream decodes as independent mono substreams, frame by frame in channel order, the
// last frame of each substream padded to its share of the superframe; the PCM is interleaved.
// Synthetic mono blocks (fxpw 29d32a5c), checked against one LibAtrac9 decoder per channel.
void TestAt9ExtendedDecode() {
  using Libs::Audio::Ajm::AjmAt9Decoder;
  using Libs::Audio::Ajm::AjmSampleEncoding;
  struct Case {
    uint32_t channels, exponent;
  };
  for (const auto test : {Case{2, 0}, Case{4, 2}, Case{12, 2}, Case{36, 2}}) {
    constexpr uint32_t samples = 256, frame_bytes = 64, block_bytes = 36, superframes = 2;
    const uint32_t frames = 1u << test.exponent;
    const uint32_t share = frame_bytes * frames;
    const std::array<uint8_t, 4> mono_config{0xfe, 0x70, 0x07,
                                             static_cast<uint8_t>(0xe0 | (test.exponent << 3))};
    using Handle = std::unique_ptr<void, decltype(&Atrac9ReleaseHandle)>;
    std::vector<Handle> references;
    for (uint32_t channel = 0; channel < test.channels; ++channel) {
      references.emplace_back(Atrac9GetHandle(), Atrac9ReleaseHandle);
      CHECK(Atrac9InitDecoder(references.back().get(),
                              const_cast<uint8_t *>(mono_config.data())) == 0);
    }
    std::vector<uint8_t> encoded;
    std::vector<float> expected(static_cast<size_t>(superframes) * frames * samples * test.channels);
    for (uint32_t superframe = 0; superframe < superframes; ++superframe) {
      for (uint32_t frame = 0; frame < frames; ++frame) {
        for (uint32_t channel = 0; channel < test.channels; ++channel) {
          std::array<uint8_t, 512> block{0, 0, 0x04, 0x20, 0x04, 0xc0, 0, 0, 0, 0xc0};
          block[8] = static_cast<uint8_t>(channel * 4);
          if (frame != 0) {
            block[0] |= 0x80;
          }
          std::array<float, samples> mono{};
          int used = 0;
          CHECK(Atrac9DecodeF32(references[channel].get(), block.data(), mono.data(), &used, 0) == 0);
          CHECK(used == static_cast<int>(block_bytes));
          for (uint32_t sample = 0; sample < samples; ++sample) {
            expected[((superframe * frames + frame) * samples + sample) * test.channels + channel] =
                mono[sample];
          }
          const uint32_t bytes = frame + 1 == frames ? share - frame * block_bytes : block_bytes;
          encoded.insert(encoded.end(), block.begin(), block.begin() + bytes);
        }
      }
    }
    CHECK(std::ranges::any_of(expected, [](float value) { return std::abs(value) > 1e-6f; }));
    AjmAt9Decoder decoder(1, 48000, AjmSampleEncoding::Float, 0);
    CHECK(Initialize(decoder, Extended(test.channels, frame_bytes, test.exponent)) == 0);
    std::vector<float> actual(expected.size() + 2, -1234.0f);
    const auto result = decoder.Decode(encoded.data(), encoded.size(), actual.data() + 1,
                                       expected.size() * sizeof(float), true, nullptr);
    CHECK(result.result == 0);
    CHECK(result.input_consumed == encoded.size());
    CHECK(result.output_written == expected.size() * sizeof(float));
    CHECK(result.frames == superframes * frames);
    CHECK(result.format.channel_num == test.channels);
    CHECK(actual.front() == -1234.0f && actual.back() == -1234.0f);
    bool equal = true;
    for (size_t sample = 0; sample < expected.size(); ++sample) {
      equal = equal && std::abs(actual[sample + 1] - expected[sample]) < 1e-6f;
    }
    CHECK(equal);
  }
}

} // namespace

int main() {
  TestAt9ParseConfigData();
  TestAt9DecoderConfigs();
  TestAt9ExtendedDecode();
  if (failures != 0) {
    std::fprintf(stderr, "AjmTests: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("AjmTests: ok\n");
  return 0;
}
