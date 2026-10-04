// readWav16k: what the recorders write -> 16 kHz mono floats.

#include <logos_test.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "voice.h"

namespace {
void put32(std::vector<unsigned char>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 255); }
void put16(std::vector<unsigned char>& v, uint16_t x) { v.push_back(x & 255); v.push_back(x >> 8); }

std::string writeWav(int rate, int channels, const std::vector<int16_t>& samples, bool unfinishedHeader) {
    std::vector<unsigned char> d;
    const uint32_t dataLen = static_cast<uint32_t>(samples.size() * 2);
    d.insert(d.end(), {'R', 'I', 'F', 'F'}); put32(d, 36 + dataLen);
    d.insert(d.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '}); put32(d, 16);
    put16(d, 1); put16(d, channels); put32(d, rate); put32(d, rate * channels * 2); put16(d, channels * 2); put16(d, 16);
    d.insert(d.end(), {'d', 'a', 't', 'a'}); put32(d, unfinishedHeader ? 0 : dataLen);
    for (int16_t s : samples) put16(d, static_cast<uint16_t>(s));
    const std::string path = "/tmp/basecamp_voice_test_" + std::to_string(rate) + "_" + std::to_string(channels) + ".wav";
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(d.data(), 1, d.size(), f);
    std::fclose(f);
    return path;
}
}

LOGOS_TEST(wav_16k_mono_is_read_as_is) {
    std::vector<float> pcm;
    std::string err;
    LOGOS_ASSERT_TRUE(readWav16k(writeWav(16000, 1, {0, 16384, -16384, 32767}, false), &pcm, &err));
    LOGOS_ASSERT_EQ(pcm.size(), size_t(4));
    LOGOS_ASSERT_TRUE(pcm[1] > 0.49f && pcm[1] < 0.51f);
    LOGOS_ASSERT_TRUE(pcm[2] < -0.49f && pcm[2] > -0.51f);
}

LOGOS_TEST(wav_48k_stereo_is_downmixed_and_resampled) {
    std::vector<int16_t> s;
    for (int i = 0; i < 4800; ++i) { s.push_back(8192); s.push_back(-8192 + 16384); }  // L=0.25, R=0.25
    std::vector<float> pcm;
    std::string err;
    LOGOS_ASSERT_TRUE(readWav16k(writeWav(48000, 2, s, false), &pcm, &err));
    LOGOS_ASSERT_EQ(pcm.size(), size_t(1600));
    LOGOS_ASSERT_TRUE(pcm[800] > 0.24f && pcm[800] < 0.26f);
}

LOGOS_TEST(wav_with_unfinished_length_uses_the_rest_of_the_file) {
    std::vector<float> pcm;
    std::string err;
    LOGOS_ASSERT_TRUE(readWav16k(writeWav(16000, 1, std::vector<int16_t>(800, 100), true), &pcm, &err));
    LOGOS_ASSERT_EQ(pcm.size(), size_t(800));
    LOGOS_ASSERT_FALSE(readWav16k("/nonexistent.wav", &pcm, &err));
}
