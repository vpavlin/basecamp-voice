#include "basecamp_voice_core_impl.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <mutex>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <thread>

#include <nlohmann/json.hpp>
#include "logos_sdk.h"

#include "assets.h"
#include "bus.h"
#include "engine.h"
#include "llm.h"
#include "model_planner.h"
#include "planner.h"
#include "text.h"
#include "voice.h"

using Json = nlohmann::json;

namespace {
const char* kVersion = "0.2.0";

// Where the model runs unless the user chooses: the CPU on Linux (an iGPU
// shared with the desktop was barely faster and reset twice; ADR 0007), the
// GPU on macOS (Metal on Apple Silicon is fast and dependable).
#if defined(__APPLE__)
const char* kDefaultDevice = "gpu";
#else
const char* kDefaultDevice = "cpu";
#endif

std::string failJson(const std::string& error) { return safeDump(Json{{"ok", false}, {"error", error}}); }

void mkdirs(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        if (path[i] == '/' && cur.size() > 1) ::mkdir(cur.c_str(), 0755);
    }
    ::mkdir(path.c_str(), 0755);
}
}  // namespace

// By-name calls into any module (docs/adr/0002). Only the engine's worker
// thread calls this, so the LpClient cache in modules() sees one thread.
class DynamicBus : public Bus {
public:
    LogosModules* modules = nullptr;

    CallResult invoke(const std::string& module, const std::string& method,
                      const Json& args, int timeoutMs) override {
        CallResult r;
        if (!modules) { r.error = "the core is not connected to Basecamp yet"; return r; }
        logos::CallError err;
        Json value = modules->dynamic(module).invoke(method, args, &err, timeoutMs);
        if (!err.code.empty()) {
            r.error = err.code == "object_unavailable" ? module + " is not running" : err.code + ": " + err.message;
            return r;
        }
        r.ok = true;
        r.value = value;
        return r;
    }

    struct LpSub : Subscription {
        logos::LpSubscription sub;
    };

    std::unique_ptr<Subscription> subscribe(const std::string& module, const std::string& event,
                                            std::function<void(const Json& args)> cb) override {
        if (!modules) return nullptr;
        auto s = std::make_unique<LpSub>();
        s->sub = modules->dynamic(module).subscribe(event, [cb](Json args) { if (cb) cb(args); });
        return s;
    }
};

// Everything that is not the engine: settings, downloads, the model server,
// the planner choice, and the microphone-to-text path.
class Voice : public Planner {
public:
    Engine* engine = nullptr;
    std::string dir;
    std::unique_ptr<Assets> assets;
    std::unique_ptr<LlamaServer> llama;
    Recorder recorder;
    Transcriber transcriber;

    std::mutex mu;
    Json settings = {{"endpoint", ""}, {"model", ""}, {"apiKey", ""}, {"localModel", "qwen3-4b"}, {"device", kDefaultDevice}};

    // The runtime is the Vulkan build: libggml-vulkan.so sits beside the
    // program (the unpacked folder name does not say).
    bool gpuBuild() {
        if (!assets || !assets->have("runtime")) return false;
        const std::string dir = assets->path("runtime").substr(0, assets->path("runtime").rfind('/'));
        return ::access((dir + "/libggml-vulkan.so").c_str(), F_OK) == 0 ||
               ::access((dir + "/libggml-metal.dylib").c_str(), F_OK) == 0;
    }

    // "cpu" (the default: steadier; an iGPU shared with the rest of the desktop
    // was barely faster and reset twice in testing) or "gpu".
    bool allowGpu() {
        std::lock_guard<std::mutex> lk(mu);
        return settings.value("device", std::string(kDefaultDevice)) == "gpu";
    }
    std::string voiceState = "idle";     // idle | recording | transcribing
    std::string voiceError;
    std::string lastTranscript;
    std::string plannerNote;
    std::thread worker;
    std::atomic<bool> stopping{false};
    std::atomic<bool> warming{false};

    // With the local model set up, start the server and read the system
    // prompt into its cache as soon as the view is open, so the first command
    // only pays for what was said. Runs on the engine's worker thread.
    void warmUp() {
        if (mode() != "local" || !llama || llama->state() != "stopped" || warming.exchange(true)) return;
        engine->post([this]() {
            std::string base, err;
            if (llama->ensure(assets->path("runtime"), assets->path("llm"), &base, &err, [this] { return stopping.load(); }, allowGpu())) {
                ChatEndpoint ep;
                ep.baseUrl = base;
                const Json messages = Json::array({{{"role", "system"}, {"content", ModelPlanner::systemPrompt()}},
                                                   {{"role", "user"}, {"content", "Context:\nInstalled apps: none\n\nUser: \"hello\""}}});
                std::string content;
                chatJson(ep, messages, ModelPlanner::schema(), 300, &content, &err, [this] { return stopping.load(); });
            }
            warming = false;
        });
    }

    void ready(const std::string& dataDir) {
        dir = dataDir;
        mkdirs(dir);
        assets = std::make_unique<Assets>(dir + "/assets");
        llama = std::make_unique<LlamaServer>(dir);
        std::ifstream f(dir + "/settings.json");
        Json s = Json::parse(f, nullptr, false);
        if (s.is_object()) for (const auto& k : {"endpoint", "model", "apiKey", "localModel", "device"}) if (s.contains(k) && s[k].is_string()) settings[k] = s[k];
        std::string err;
        if (!assets->setLlmChoice(settings["localModel"], &err)) settings["localModel"] = assets->llmChoice();
    }

    bool isReady() const { return assets != nullptr; }

    Json settingsShown() {
        std::lock_guard<std::mutex> lk(mu);
        Json s = settings;
        s["apiKey"] = settings["apiKey"].get<std::string>().empty() ? "" : "(set)";
        return s;
    }

    std::string endpoint() {
        std::lock_guard<std::mutex> lk(mu);
        return settings["endpoint"].get<std::string>();
    }

    // Which planner answers: a configured endpoint, the local model, or (until
    // the models are downloaded) the fixed phrases.
    std::string mode() {
        if (!endpoint().empty()) return "remote";
        if (assets && assets->have("runtime") && assets->have("llm")) return "local";
        return "rules";
    }

    bool continues() const override { return const_cast<Voice*>(this)->mode() != "rules"; }

    nlohmann::json planNext(const std::string& text, const nlohmann::json& done) override {
        return planWith(text, &done);
    }

    nlohmann::json plan(const std::string& text) override { return planWith(text, nullptr); }

    nlohmann::json planWith(const std::string& text, const nlohmann::json* done) {
        const std::string m = mode();
        if (m == "rules") {
            Json p = RulePlanner().plan(text);
            if (!p.value("ok", false))
                p["error"] = p.value("error", std::string()) + " For free-form requests, download the models in Setup.";
            return p;
        }
        ChatEndpoint ep;
        if (m == "remote") {
            std::lock_guard<std::mutex> lk(mu);
            ep.baseUrl = settings["endpoint"];
            ep.model = settings["model"];
            ep.apiKey = settings["apiKey"];
        } else {
            std::string err;
            if (!llama->ensure(assets->path("runtime"), assets->path("llm"), &ep.baseUrl, &err,
                               [this] { return stopping.load(); }, allowGpu()))
                return {{"ok", false}, {"error", "The language model could not start: " + err}};
        }
        ModelPlanner planner(
            [this](const std::string& t) { return engine->tools().context(t); },
            [this, ep](const Json& messages, const Json& schema, std::string* content, std::string* error, Json* stats) {
                return chatJson(ep, messages, schema, 300, content, error, [this] { return stopping.load(); }, stats);
            },
            [this]() { return engine->history(6); },
            [this]() { return engine->tools().intents(); });
        Json result = done ? planner.planNext(text, *done) : planner.plan(text);
        // The local server died mid-request (a GPU driver reset, say): start it
        // again, on the CPU if it was the GPU, and ask once more.
        const std::string err0 = result.contains("error") && result["error"].is_string() ? result["error"].get<std::string>() : "";
        const bool gpuError = err0.find("DeviceLost") != std::string::npos || err0.find("vk::") != std::string::npos;
        if (m == "local" && !result.value("ok", false) && gpuError) llama->abandonGpu();
        if (m == "local" && !result.value("ok", false) && (gpuError || llama->crashed())) {
            std::string err;
            if (llama->ensure(assets->path("runtime"), assets->path("llm"), &ep.baseUrl, &err,
                              [this] { return stopping.load(); }, allowGpu())) {
                ModelPlanner again(
                    [this](const std::string& t) { return engine->tools().context(t); },
                    [ep](const Json& messages, const Json& schema, std::string* content, std::string* error, Json* stats) {
                        return chatJson(ep, messages, schema, 300, content, error, {}, stats);
                    },
                    [this]() { return engine->history(6); },
                    [this]() { return engine->tools().intents(); });
                result = done ? again.planNext(text, *done) : again.plan(text);
            }
        }
        return result;
    }

    Json state() {
        Json s;
        {
            std::lock_guard<std::mutex> lk(mu);
            s["voice"] = {{"state", voiceState}, {"error", voiceError}, {"transcript", lastTranscript}};
        }
        s["setup"] = assets ? assets->status() : Json{{"state", "missing"}};
        s["model"] = {{"mode", mode()}, {"server", llama ? llama->state() : "stopped"},
                      {"gpu", llama ? llama->onGpu() : false},
                      {"gpuAvailable", gpuBuild()}};
        s["settings"] = settingsShown();
        if (assets) s["models"] = assets->llmChoices();
        return s;
    }

    // ---- the microphone: idle -> recording -> transcribing -> idle ----------
    std::mutex workerMu;                  // the transcription thread
    long long recordingSince = 0;
    static constexpr long long kMaxRecordingMs = 120000;

    static long long nowMs() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    std::string startRecording() {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (voiceState != "idle") return failJson(voiceState == "recording" ? "Already listening." : "Still working on the last recording.");
            voiceState = "recording";
            voiceError.clear();
            recordingSince = nowMs();
        }
        std::string err;
        if (!recorder.start(dir, &err)) {
            std::lock_guard<std::mutex> lk(mu);
            voiceState = "idle";
            voiceError = err;
            return failJson(err);
        }
        return safeDump(Json{{"ok", true}});
    }

    std::string stopRecording() {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (voiceState != "recording") return failJson("Not listening.");
        }
        const std::string wav = recorder.stop();
        if (!recorder.lastError().empty()) {
            std::lock_guard<std::mutex> lk(mu);
            voiceState = "idle";
            voiceError = recorder.lastError();
            return failJson(voiceError);
        }
        return transcribeAndSubmit(wav, true);
    }

    std::string cancelRecording() {
        std::lock_guard<std::mutex> lk(mu);
        if (voiceState != "recording") return safeDump(Json{{"ok", true}});
        recorder.cancel();
        voiceState = "idle";
        return safeDump(Json{{"ok", true}});
    }

    // A forgotten recording stops itself (checked on every poll).
    void limitRecording() {
        bool tooLong = false;
        {
            std::lock_guard<std::mutex> lk(mu);
            tooLong = voiceState == "recording" && nowMs() - recordingSince > kMaxRecordingMs;
        }
        if (tooLong) stopRecording();
    }

    // Transcribe off the caller's thread, then submit what was said.
    // fromRecording: the state is "recording" now (else it must be idle).
    std::string transcribeAndSubmit(const std::string& wav, bool fromRecording) {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (!assets->have("stt")) {
                if (fromRecording) voiceState = "idle";
                voiceError = "Download the speech model first (Setup).";
                return failJson(voiceError);
            }
            if (voiceState != (fromRecording ? "recording" : "idle"))
                return failJson("Still working on the last recording.");
            voiceState = "transcribing";
            voiceError.clear();
        }
        std::lock_guard<std::mutex> wl(workerMu);
        if (worker.joinable()) worker.join();   // the previous one has finished: the state was not transcribing
        worker = std::thread([this, wav]() {
            std::string text, err;
            bool ok = false;
            try {
                ok = transcriber.transcribe(assets->path("stt"), wav, &text, &err);
            } catch (const std::exception& e) {
                err = std::string("speech recognition failed: ") + e.what();
            }
            {
                std::lock_guard<std::mutex> lk(mu);
                voiceState = "idle";
                if (!ok) { voiceError = err; return; }
                lastTranscript = text;
            }
            if (engine) engine->submit(text);
        });
        return safeDump(Json{{"ok", true}});
    }

    void beginShutdown() {
        stopping = true;
        if (assets) assets->cancel();
        if (llama) llama->interrupt();
    }

    void shutdown() {
        stopping = true;
        recorder.cancel();
        if (assets) assets->cancel();
        {
            std::lock_guard<std::mutex> wl(workerMu);
            if (worker.joinable()) worker.join();
        }
        if (llama) llama->stop();
    }
};

BasecampVoiceCoreImpl::BasecampVoiceCoreImpl()
    : m_bus(std::make_unique<DynamicBus>()),
      m_voice(std::make_unique<Voice>()),
      m_engine(std::make_unique<Engine>(*m_bus, *m_voice)) {
    m_voice->engine = m_engine.get();
}

BasecampVoiceCoreImpl::~BasecampVoiceCoreImpl() {
    // Tell everything that can wait to stop waiting before joining the worker.
    if (m_voice) m_voice->beginShutdown();
    if (m_engine) m_engine->stop();
    if (m_voice) m_voice->shutdown();
}

void BasecampVoiceCoreImpl::onContextReady() {
    std::string dir = instancePersistencePath();
    if (dir.empty()) {
        const char* home = std::getenv("HOME");
        dir = std::string(home ? home : "/tmp") + "/.local/share/basecamp-voice";
    }
    m_voice->ready(dir);
}

LogosShutdown BasecampVoiceCoreImpl::aboutToUnload() {
    m_voice->beginShutdown();
    m_engine->stop();
    m_voice->shutdown();
    return LogosShutdown::Synchronous;
}

std::string BasecampVoiceCoreImpl::state(long long since, bool takeViewAction) {
    Json s = m_engine->events(since, takeViewAction);
    if (m_voice->isReady()) s.update(m_voice->state());
    s["version"] = kVersion;
    return safeDump(s);
}

#define GUARD(...) \
    try { if (!m_voice->isReady()) return failJson("Basecamp Voice is still starting."); __VA_ARGS__ } \
    catch (const std::exception& e) { return failJson(e.what()); }

std::string BasecampVoiceCoreImpl::submit(const std::string& text) {
    GUARD({
        if (!m_bus->modules) m_bus->modules = &modules();
        return safeDump(m_engine->submit(text));
    })
}

std::string BasecampVoiceCoreImpl::confirm(const std::string& jobId) { GUARD({ return safeDump(m_engine->confirm(jobId)); }) }

std::string BasecampVoiceCoreImpl::cancel(const std::string& jobId) { GUARD({ return safeDump(m_engine->cancel(jobId)); }) }

std::string BasecampVoiceCoreImpl::events(const std::string& sinceSeq) {
    GUARD({
        // The view polls while open: a good moment to get the model ready.
        if (!m_bus->modules) m_bus->modules = &modules();
        m_voice->warmUp();
        m_voice->limitRecording();
        return state(sinceSeq.empty() ? 0 : std::atoll(sinceSeq.c_str()), true);
    })
}

std::string BasecampVoiceCoreImpl::viewActionDone(const std::string& actionId, const std::string& resultJson) {
    GUARD({ return safeDump(m_engine->viewActionDone(actionId, resultJson)); })
}

std::string BasecampVoiceCoreImpl::newConversation() {
    GUARD({ return safeDump(m_engine->newConversation()); })
}

std::string BasecampVoiceCoreImpl::snapshot() {
    GUARD({
        Json s = Json::parse(state(1LL << 60, false), nullptr, false);
        if (s.is_discarded()) return failJson("internal error");
        s.erase("events");
        return safeDump(s);
    })
}

std::string BasecampVoiceCoreImpl::startSetup() {
    GUARD({
        std::vector<std::string> ids = {"stt"};
        if (m_voice->endpoint().empty()) { ids.push_back("runtime"); ids.push_back("llm"); }
        std::string err;
        if (!m_voice->assets->start(ids, &err)) return failJson(err);
        return Json{{"ok", true}}.dump();
    })
}

std::string BasecampVoiceCoreImpl::cancelSetup() {
    GUARD({ m_voice->assets->cancel(); return Json{{"ok", true}}.dump(); })
}

std::string BasecampVoiceCoreImpl::recordStart() {
    GUARD({
        if (!m_bus->modules) m_bus->modules = &modules();
        return m_voice->startRecording();
    })
}

std::string BasecampVoiceCoreImpl::recordStop() {
    GUARD({
        return m_voice->stopRecording();
    })
}

std::string BasecampVoiceCoreImpl::recordCancel() {
    GUARD({
        return m_voice->cancelRecording();
    })
}

std::string BasecampVoiceCoreImpl::submitRecording(const std::string& wavPath) {
    GUARD({
        if (!m_bus->modules) m_bus->modules = &modules();
        return m_voice->transcribeAndSubmit(wavPath, false);
    })
}

std::string BasecampVoiceCoreImpl::configure(const std::string& settingsJson) {
    GUARD({
        if (!settingsJson.empty()) {
            Json in = Json::parse(settingsJson, nullptr, false);
            if (!in.is_object()) return failJson("Settings must be a JSON object.");
            if (in.contains("localModel") && in["localModel"].is_string()) {
                std::string err;
                if (!m_voice->assets->setLlmChoice(in["localModel"], &err)) return failJson(err);
            }
            std::lock_guard<std::mutex> lk(m_voice->mu);
            // A key belongs to the server it was given for: a new endpoint
            // without a new key drops the old one.
            const bool newEndpoint = in.contains("endpoint") && in["endpoint"].is_string() &&
                                     in["endpoint"] != m_voice->settings["endpoint"];
            if (newEndpoint && !(in.contains("apiKey") && in["apiKey"].is_string())) m_voice->settings["apiKey"] = "";
            for (const auto& k : {"endpoint", "model", "apiKey", "localModel", "device"})
                if (in.contains(k) && in[k].is_string()) m_voice->settings[k] = in[k];
            // Owner-only: it may hold an API key.
            const std::string path = m_voice->dir + "/settings.json", tmp = path + ".tmp";
            const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd >= 0) {
                const std::string body = m_voice->settings.dump(2);
                const bool written = ::write(fd, body.data(), body.size()) == static_cast<ssize_t>(body.size());
                ::close(fd);
                if (written) ::rename(tmp.c_str(), path.c_str());
            }
        }
        Json out = {{"ok", true}, {"settings", m_voice->settingsShown()}};
        return safeDump(out);
    })
}
