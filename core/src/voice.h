#pragma once

#include <mutex>
#include <string>
#include <sys/types.h>
#include <vector>

// Records from the default microphone with the system's recorder
// (pw-record, then parecord, then arecord) into a 16 kHz mono WAV. Basecamp
// ships no Qt Multimedia, and the view's sandbox cannot record.
class Recorder {
public:
    ~Recorder();
    bool start(const std::string& dir, std::string* error);
    // Stops so the recorder finishes the WAV header; returns the file.
    std::string stop();
    void cancel();
    bool active() const;

private:
    mutable std::mutex m_mu;
    pid_t m_pid = -1;
    std::string m_path;
};

struct parakeet_context;

// Speech to text with Parakeet (whisper.cpp), in this process. The model is
// loaded on first use and kept.
class Transcriber {
public:
    ~Transcriber();
    bool transcribe(const std::string& model, const std::string& wav, std::string* text, std::string* error);

private:
    std::mutex m_mu;
    parakeet_context* m_ctx = nullptr;
    std::string m_loaded;
};

// At most this long a file: about 30 minutes of 16 kHz mono PCM16.
constexpr long long kMaxWavBytes = 64LL * 1024 * 1024;

// PCM16 or float32 WAV, 8-192 kHz, 1-8 channels -> 16 kHz mono floats.
bool readWav16k(const std::string& path, std::vector<float>* pcm, std::string* error);
