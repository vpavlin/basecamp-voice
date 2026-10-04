#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <sys/types.h>

#include <nlohmann/json.hpp>

// The local model server: llama.cpp's llama-server, started on demand on a
// free localhost port, kept while in use, stopped with the module. llama-server
// sleeps on its own after a while idle (--sleep-idle-seconds), giving the
// memory back.
class LlamaServer {
public:
    explicit LlamaServer(std::string dataDir);
    ~LlamaServer();

    // Starts the server for this program and model if it is not running, and
    // waits until it answers. Fills baseUrl ("http://127.0.0.1:<port>").
    // Call it from the engine's worker thread: the server is tied to the
    // thread that starts it (proc::spawnTied).
    // allowGpu: false runs it on the CPU (the user's choice); a server running
    // on the other device is restarted.
    bool ensure(const std::string& program, const std::string& model, std::string* baseUrl,
                std::string* error, const std::function<bool()>& stopping, bool allowGpu = true);
    // Makes a waiting ensure() give up (module unload).
    void interrupt();
    void stop();
    // "stopped" | "starting" | "ready" | "failed: ..."
    std::string state() const;
    // True when the running server uses the GPU.
    bool onGpu() const;
    // True when the server process has died since it was ready.
    bool crashed();
    // The GPU failed mid-request (e.g. "device lost"): stop the server; the
    // next ensure() starts it on the CPU for the rest of this session.
    void abandonGpu();

private:
    void killStale();
    void setState(const std::string& s, bool gpu);
    bool start(const std::string& program, const std::string& model, bool gpu, std::string* error);

    std::string m_dir;
    std::mutex m_mu;              // the process: m_pid, m_port, m_model, m_gpuFailed
    pid_t m_pid = -1;
    int m_port = 0;
    std::string m_model;
    bool m_gpuFailed = false;
    std::atomic<bool> m_interrupted{false};

    mutable std::mutex m_stateMu; // what the view reads, never held for long
    std::string m_state = "stopped";
    bool m_gpu = false;
};

struct ChatEndpoint {
    std::string baseUrl;     // e.g. http://127.0.0.1:8080 or http://localhost:11434
    std::string model;       // empty for llama-server
    std::string apiKey;      // optional bearer token
};

// One OpenAI-compatible chat completion whose reply must match `schema`.
// Fills content with the assistant's message.
bool chatJson(const ChatEndpoint& ep, const nlohmann::json& messages, const nlohmann::json& schema,
              int timeoutSec, std::string* content, std::string* error,
              const std::function<bool()>& abort = {}, nlohmann::json* stats = nullptr);
