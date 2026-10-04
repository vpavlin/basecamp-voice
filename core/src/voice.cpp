#include "voice.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>

#if defined(__APPLE__) && defined(BASECAMP_VOICE_MAC_RECORDER)
#include "recorder_blob.h"
#endif

#include "parakeet.h"
#include "proc.h"

// ---- recording ----------------------------------------------------------------

Recorder::~Recorder() { cancel(); }

namespace {

bool fileExists(const std::string& p) { return ::access(p.c_str(), F_OK) == 0; }

std::string readSmall(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return {};
    char buf[1024];
    const size_t n = std::fread(buf, 1, sizeof buf, f);
    std::fclose(f);
    return std::string(buf, n);
}

void removeAll(const std::string& wav) {
    for (const char* ext : {"", ".stop", ".started", ".done", ".err"}) ::unlink((wav + ext).c_str());
}

#if defined(__APPLE__) && defined(BASECAMP_VOICE_MAC_RECORDER)
// docs/macos.md: Basecamp's macOS app declares no microphone use, and macOS
// kills any of its processes that opens the microphone. So recording is done
// by a tiny app of our own, launched with `open` so that macOS treats it as
// responsible for itself and asks the user under its own name.
const char* kRecorderPlist = R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>co.logos.basecamp-voice.recorder</string>
<key>CFBundleName</key><string>Basecamp Voice Recorder</string>
<key>CFBundleDisplayName</key><string>Basecamp Voice Recorder</string>
<key>CFBundleExecutable</key><string>BasecampVoiceRecorder</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>1</string>
<key>LSMinimumSystemVersion</key><string>12.0</string>
<key>LSUIElement</key><true/>
<key>NSMicrophoneUsageDescription</key><string>Basecamp Voice records what you say so it can turn it into a request for Basecamp. The recording stays on this Mac.</string>
</dict></plist>
)";

bool writeFile(const std::string& p, const void* data, size_t n) {
    FILE* f = std::fopen(p.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data, 1, n, f) == n;
    std::fclose(f);
    return ok;
}

// The helper bundle in the data directory, written (and ad-hoc signed, which
// seals its Info.plist) when missing or when this build carries a new one.
bool ensureHelper(const std::string& dir, std::string* app, std::string* error) {
    *app = dir + "/Basecamp Voice Recorder.app";
    const std::string exe = *app + "/Contents/MacOS/BasecampVoiceRecorder";
    const std::string stamp = *app + "/Contents/.size";
    const std::string want = std::to_string(kRecorderBlobSize);
    if (fileExists(exe) && readSmall(stamp) == want) return true;
    for (const std::string& d : {*app, *app + "/Contents", *app + "/Contents/MacOS"}) ::mkdir(d.c_str(), 0755);
    if (!writeFile(*app + "/Contents/Info.plist", kRecorderPlist, std::strlen(kRecorderPlist)) ||
        !writeFile(exe, kRecorderBlob, kRecorderBlobSize)) {
        *error = "could not write the recorder app into " + dir;
        return false;
    }
    ::chmod(exe.c_str(), 0755);
    std::string err;
    if (proc::run({"/usr/bin/codesign", "--force", "--sign", "-", *app}, "", 30000, &err) != 0) {
        *error = "could not sign the recorder app" + (err.empty() ? std::string() : ": " + err);
        return false;
    }
    writeFile(stamp, want.data(), want.size());
    return true;
}
#endif

}  // namespace

bool Recorder::start(const std::string& dir, std::string* error) {
    std::lock_guard<std::mutex> lk(m_mu);
    if (m_pid > 0 || m_helper) { *error = "already recording"; return false; }
    const std::string path = dir + "/recording.wav";
    removeAll(path);
    m_error.clear();
#if defined(__APPLE__)
#if defined(BASECAMP_VOICE_MAC_RECORDER)
    std::string app;
    if (!ensureHelper(dir, &app, error)) return false;
    std::string err;
    // -n: a fresh instance; -g: do not bring it to the front.
    if (proc::run({"/usr/bin/open", "-n", "-g", "-a", app, "--args", path, path + ".stop"}, "", 15000, &err) != 0) {
        *error = "could not start the recorder app" + (err.empty() ? std::string() : ": " + err);
        return false;
    }
    m_helper = true;
    m_path = path;
    return true;
#else
    *error = "recording is not available in this build; type instead";
    return false;
#endif
#else
    const std::vector<std::vector<std::string>> tries = {
        {"pw-record", "--rate", "16000", "--channels", "1", path},
        {"parecord", "--file-format=wav", "--rate=16000", "--channels=1", path},
        {"arecord", "-q", "-f", "S16_LE", "-r", "16000", "-c", "1", path},
    };
    std::string why = "no recorder found (pw-record, parecord or arecord)";
    for (const auto& argv : tries) {
        std::string err;
        const pid_t pid = proc::spawn(argv, "", {}, &err);
        if (pid < 0) continue;
        // A recorder that cannot open the microphone exits at once.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        if (!proc::alive(pid)) { proc::stop(pid, SIGKILL, 100); why = argv[0] + " could not record (is there a microphone?)"; continue; }
        m_pid = pid;
        m_path = path;
        return true;
    }
    *error = why;
    return false;
#endif
}

std::string Recorder::stop() {
    pid_t pid;
    bool helper;
    std::string path;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        pid = m_pid;
        helper = m_helper;
        path = m_path;
        m_pid = -1;
        m_helper = false;
    }
    if (pid > 0) proc::stop(pid, SIGINT, 3000);
    if (helper) {
        // Ask the helper to stop, then wait for it to finish the file (or say why not).
        FILE* f = std::fopen((path + ".stop").c_str(), "w");
        if (f) std::fclose(f);
        for (int i = 0; i < 100 && !fileExists(path + ".done") && !fileExists(path + ".err"); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::lock_guard<std::mutex> lk(m_mu);
        if (fileExists(path + ".err")) m_error = readSmall(path + ".err");
        else if (!fileExists(path + ".started")) m_error = "The recorder did not start. If macOS asked about the microphone, allow it and try again.";
    }
    return path;
}

void Recorder::cancel() {
    const std::string path = stop();
    if (!path.empty()) removeAll(path);
}

bool Recorder::active() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_pid > 0 || m_helper;
}

std::string Recorder::lastError() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_error;
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
