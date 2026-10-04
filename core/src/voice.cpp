#include "voice.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>

#include "parakeet.h"
#include "proc.h"

// ---- recording ----------------------------------------------------------------

Recorder::~Recorder() { cancel(); }

bool Recorder::start(const std::string& dir, std::string* error) {
    std::lock_guard<std::mutex> lk(m_mu);
    if (m_pid > 0) { *error = "already recording"; return false; }
    const std::string path = dir + "/recording.wav";
    ::unlink(path.c_str());
    const std::vector<std::vector<std::string>> tries = {
        {"pw-record", "--rate", "16000", "--channels", "1", path},
        {"parecord", "--file-format=wav", "--rate=16000", "--channels=1", path},
        {"arecord", "-q", "-f", "S16_LE", "-r", "16000", "-c", "1", path},
    };
    std::string why = "no recorder found (pw-record, parecord or arecord)";
    for (const auto& argv : tries) {
        std::string err;
        const pid_t pid = proc::spawn(argv, "", {}, &err);
        if (pid < 0) { if (why.rfind("no recorder", 0) == 0) why = "no recorder found (pw-record, parecord or arecord)"; continue; }
        // A recorder that cannot open the microphone exits at once.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        if (!proc::alive(pid)) { why = argv[0] + " could not record (is there a microphone?)"; continue; }
        m_pid = pid;
        m_path = path;
        return true;
    }
    *error = why;
    return false;
}

std::string Recorder::stop() {
    pid_t pid;
    std::string path;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        pid = m_pid;
        path = m_path;
        m_pid = -1;
    }
    if (pid > 0) proc::stop(pid, SIGINT, 3000);
    return path;
}

void Recorder::cancel() {
    const std::string path = stop();
    if (!path.empty()) ::unlink(path.c_str());
}

bool Recorder::active() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_pid > 0;
}

// ---- transcription ------------------------------------------------------------------

namespace {
void quiet(enum ggml_log_level, const char*, void*) {}
}

Transcriber::~Transcriber() {
    if (m_ctx) parakeet_free(m_ctx);
}

bool Transcriber::transcribe(const std::string& model, const std::string& wav, std::string* text, std::string* error) {
    std::lock_guard<std::mutex> lk(m_mu);
    std::vector<float> pcm;
    if (!readWav16k(wav, &pcm, error)) return false;
    if (pcm.size() < 16000 / 4) { *error = "the recording is too short"; return false; }
    if (!m_ctx || m_loaded != model) {
        if (m_ctx) { parakeet_free(m_ctx); m_ctx = nullptr; }
        parakeet_log_set(quiet, nullptr);
        parakeet_context_params cp = parakeet_context_default_params();
        cp.use_gpu = false;
        m_ctx = parakeet_init_from_file_with_params(model.c_str(), cp);
        if (!m_ctx) { *error = "could not load the speech model"; return false; }
        m_loaded = model;
    }
    parakeet_full_params fp = parakeet_full_default_params(PARAKEET_SAMPLING_GREEDY);
    fp.n_threads = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    if (parakeet_full(m_ctx, fp, pcm.data(), static_cast<int>(pcm.size())) != 0) {
        *error = "speech recognition failed";
        return false;
    }
    std::string out;
    const int segments = parakeet_full_n_segments(m_ctx);
    for (int i = 0; i < segments; ++i) {
        const char* s = parakeet_full_get_segment_text(m_ctx, i);
        if (s) out += s;
    }
    // Trim.
    const size_t b = out.find_first_not_of(" \t\n");
    const size_t e = out.find_last_not_of(" \t\n");
    *text = b == std::string::npos ? std::string() : out.substr(b, e - b + 1);
    if (text->empty()) { *error = "no speech was recognised"; return false; }
    return true;
}
