#include "voice.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

// ---- WAV --------------------------------------------------------------------------

namespace {
uint32_t u32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
uint16_t u16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
}

bool readWav16k(const std::string& path, std::vector<float>* pcm, std::string* error) {
    // A regular file of sane size only (not /dev/zero, not an hour of audio).
    struct stat st;
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) { *error = "the recording is not a file"; return false; }
    if (st.st_size > kMaxWavBytes) { *error = "the recording is too long"; return false; }
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { *error = "cannot read the recording"; return false; }
    std::vector<unsigned char> d;
    unsigned char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
    std::fclose(f);
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0) {
        *error = "the recording is not a WAV file";
        return false;
    }
    int format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    size_t dataOff = 0, dataLen = 0;
    for (size_t off = 12; off + 8 <= d.size();) {
        const uint32_t len = u32(&d[off + 4]);
        if (std::memcmp(&d[off], "fmt ", 4) == 0 && off + 8 + 16 <= d.size()) {
            format = u16(&d[off + 8]);
            channels = u16(&d[off + 10]);
            rate = u32(&d[off + 12]);
            bits = u16(&d[off + 22]);
            if (format == 0xFFFE && len >= 26 && off + 8 + 26 <= d.size()) format = u16(&d[off + 8 + 24]);
        } else if (std::memcmp(&d[off], "data", 4) == 0) {
            dataOff = off + 8;
            // A recorder stopped hard may leave the length unwritten.
            dataLen = (len == 0 || len == 0xFFFFFFFF || dataOff + len > d.size()) ? d.size() - dataOff : len;
            break;
        }
        off += 8 + len + (len & 1);
    }
    if (!dataOff || channels <= 0 || rate == 0) { *error = "the recording has no audio"; return false; }
    if (channels > 8 || rate < 8000 || rate > 192000) { *error = "unsupported WAV format"; return false; }
    const bool f32 = format == 3 && bits == 32, s16 = format == 1 && bits == 16;
    if (!f32 && !s16) { *error = "unsupported WAV format"; return false; }
    const size_t frame = static_cast<size_t>(channels) * (bits / 8);
    const size_t frames = dataLen / frame;
    std::vector<float> mono(frames);
    for (size_t i = 0; i < frames; ++i) {
        float sum = 0;
        for (int c = 0; c < channels; ++c) {
            const unsigned char* p = &d[dataOff + i * frame + c * (bits / 8)];
            if (s16) sum += static_cast<int16_t>(u16(p)) / 32768.0f;
            else { float v; std::memcpy(&v, p, 4); sum += v; }
        }
        mono[i] = sum / channels;
    }
    if (rate == 16000) { *pcm = std::move(mono); return true; }
    const size_t out = static_cast<size_t>(static_cast<double>(frames) * 16000 / rate);
    pcm->resize(out);
    for (size_t i = 0; i < out; ++i) {
        const double src = static_cast<double>(i) * rate / 16000;
        const size_t a = static_cast<size_t>(src);
        const size_t b = std::min(a + 1, frames - 1);
        const float t = static_cast<float>(src - a);
        (*pcm)[i] = mono[a] * (1 - t) + mono[b] * t;
    }
    return true;
}

