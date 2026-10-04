#include "llm.h"

#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <fstream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "net.h"
#include "text.h"
#include "proc.h"

using Json = nlohmann::json;

namespace {

int freePort() {
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 0;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    int port = 0;
    socklen_t len = sizeof(a);
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 &&
        ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0)
        port = ntohs(a.sin_port);
    ::close(s);
    return port;
}

std::string readFile(const std::string& p) {
    std::ifstream f(p);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

}  // namespace

LlamaServer::LlamaServer(std::string dataDir) : m_dir(std::move(dataDir)) {}

LlamaServer::~LlamaServer() { stop(); }

std::string LlamaServer::state() const {
    std::lock_guard<std::mutex> lk(m_stateMu);
    return m_state;
}

bool LlamaServer::onGpu() const {
    std::lock_guard<std::mutex> lk(m_stateMu);
    return m_gpu;
}

void LlamaServer::setState(const std::string& s, bool gpu) {
    std::lock_guard<std::mutex> lk(m_stateMu);
    m_state = s;
    m_gpu = gpu;
}

void LlamaServer::interrupt() { m_interrupted = true; }

void LlamaServer::abandonGpu() {
    std::lock_guard<std::mutex> lk(m_mu);
    m_gpuFailed = true;
    if (m_pid > 0) proc::stop(m_pid, SIGTERM, 3000);
    m_pid = -1;
    setState("stopped", false);
}

bool LlamaServer::crashed() {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_pid > 0 && !proc::alive(m_pid);
}

// A server left over from a run that did not shut down cleanly (Basecamp
// killed): recognised by the pid file and its command line.
void LlamaServer::killStale() {
    const std::string pidFile = m_dir + "/llama-server.pid";
    const pid_t pid = static_cast<pid_t>(std::atol(readFile(pidFile).c_str()));
    if (pid > 0 && pid != m_pid &&
        readFile("/proc/" + std::to_string(pid) + "/cmdline").find("llama-server") != std::string::npos)
        proc::stop(pid, SIGTERM, 3000);
    ::unlink(pidFile.c_str());
}

// Spawns the server (m_mu held). With gpu, all layers go to the Vulkan device.
bool LlamaServer::start(const std::string& program, const std::string& model, bool gpu, std::string* error) {
    m_port = freePort();
    if (m_port == 0) { *error = "no free local port for the model server"; return false; }
    const std::string log = m_dir + "/llama-server.log";
    ::unlink(log.c_str());
    std::string err;
    std::vector<std::string> argv = {program, "-m", model, "--host", "127.0.0.1", "--port", std::to_string(m_port),
                                     "-c", "8192", "--no-webui", "--reasoning", "off", "--sleep-idle-seconds", "900",
                                     "-ngl", gpu ? "99" : "0"};
    // Short GPU submissions: an Intel iGPU's kernel driver resets the device
    // ("device lost") when one batch runs too long, which long prompts did.
    if (gpu) { argv.push_back("--ubatch-size"); argv.push_back("128"); }
    // Tied to the calling thread (the engine's worker, which lives as long as
    // the module): if Basecamp is killed, the server goes with it.
    m_pid = proc::spawnTied(argv, log, {}, &err);
    if (m_pid < 0) { *error = "could not start llama-server: " + err; return false; }
    std::ofstream(m_dir + "/llama-server.pid") << m_pid;
    m_model = model;
    return true;
}

bool LlamaServer::ensure(const std::string& program, const std::string& model, std::string* baseUrl,
                         std::string* error, const std::function<bool()>& stopping, bool allowGpu) {
    // The Vulkan build also runs on the CPU; use the GPU unless it failed before.
    const bool vulkanBuild = program.find("vulkan") != std::string::npos ||
        ::access((program.substr(0, program.rfind('/')) + "/libggml-vulkan.so").c_str(), F_OK) == 0;
    auto giveUp = [&] { return m_interrupted.load() || (stopping && stopping()); };
    std::lock_guard<std::mutex> lk(m_mu);
    for (int attempt = 0; attempt < 2; ++attempt) {
        const bool wantGpu = vulkanBuild && allowGpu && !m_gpuFailed;
        if (m_pid > 0 && proc::alive(m_pid) && m_model == model && state() == "ready" && onGpu() == wantGpu) {
            *baseUrl = "http://127.0.0.1:" + std::to_string(m_port);
            return true;
        }
        // A server that was serving and died: on the GPU that is usually the
        // driver giving up (e.g. "device lost" with several GPU users), so the
        // rest of this session runs on the CPU.
        if (m_pid > 0 && !proc::alive(m_pid) && state() == "ready" && onGpu()) m_gpuFailed = true;
        // proc::alive does not reap, so this pid is still ours to stop.
        if (m_pid > 0) { proc::stop(m_pid, SIGTERM, 3000); m_pid = -1; }
        killStale();
        const bool gpu = vulkanBuild && allowGpu && !m_gpuFailed;
        if (!start(program, model, gpu, error)) { setState("failed: " + *error, false); return false; }
        setState("starting", gpu);
        const pid_t pid = m_pid;
        const std::string url = "http://127.0.0.1:" + std::to_string(m_port);
        const std::string log = m_dir + "/llama-server.log";

        // Loading a few GB from disk takes seconds; give it up to three minutes.
        bool exited = false, ready = false;
        for (int i = 0; i < 360 && !ready; ++i) {
            if (giveUp()) {
                proc::stop(pid, SIGTERM, 3000);
                m_pid = -1;
                setState("stopped", false);
                *error = "stopped";
                return false;
            }
            if (!proc::alive(pid)) { exited = true; break; }
            long status = 0;
            std::string body, e;
            ready = net::request("GET", url + "/health", "", {}, 2, &status, &body, &e) && status == 200;
            if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (ready) {
            setState("ready", gpu);
            *baseUrl = url;
            return true;
        }
        // Exited or hung: either way it is gone before anything else happens.
        proc::stop(pid, SIGTERM, 3000);
        m_pid = -1;
        if (gpu) {
            // A driver that cannot run it (or hangs): try again on the CPU.
            m_gpuFailed = true;
            continue;
        }
        const std::string tail = utf8Suffix(readFile(log), 400);
        *error = exited ? "the model server exited while starting: " + tail
                        : "the model server did not become ready in 3 minutes (see " + log + ")";
        setState(exited ? "failed: the model server exited while starting" : "failed: did not start in time", false);
        return false;
    }
    *error = "the model server could not start";
    setState("failed: could not start", false);
    return false;
}

void LlamaServer::stop() {
    m_interrupted = true;
    std::lock_guard<std::mutex> lk(m_mu);
    if (m_pid > 0) proc::stop(m_pid, SIGTERM, 5000);
    m_pid = -1;
    ::unlink((m_dir + "/llama-server.pid").c_str());
    setState("stopped", false);
}

bool chatJson(const ChatEndpoint& ep, const Json& messages, const Json& schema,
              int timeoutSec, std::string* content, std::string* error,
              const std::function<bool()>& abort, Json* stats) {
    Json body = {{"messages", messages}, {"temperature", 0}, {"max_tokens", 600},
                 {"response_format", {{"type", "json_schema"},
                                      {"json_schema", {{"name", "plan"}, {"strict", true}, {"schema", schema}}}}}};
    if (!ep.model.empty()) body["model"] = ep.model;
    // Plans must be quick: no thinking phase on models that have one.
    body["chat_template_kwargs"] = {{"enable_thinking", false}};
    std::vector<std::string> headers = {"Content-Type: application/json"};
    if (!ep.apiKey.empty()) headers.push_back("Authorization: Bearer " + ep.apiKey);
    std::string base = ep.baseUrl;
    while (!base.empty() && base.back() == '/') base.pop_back();
    // Accept both ".../v1" and the bare server address.
    const std::string url = base.size() >= 3 && base.compare(base.size() - 3, 3, "/v1") == 0
        ? base + "/chat/completions" : base + "/v1/chat/completions";
    long status = 0;
    std::string resp, err;
    if (!net::request("POST", url, safeDump(body), headers, timeoutSec, &status, &resp, &err, abort)) {
        *error = "could not reach the model at " + ep.baseUrl + ": " + err;
        return false;
    }
    Json r = Json::parse(resp, nullptr, false);
    if (status != 200 || r.is_discarded()) {
        std::string msg = utf8Prefix(resp, 300);
        if (!r.is_discarded() && r.is_object() && r.contains("error")) {
            const Json& e = r["error"];
            msg = e.is_object() && e.contains("message") && e["message"].is_string() ? e["message"].get<std::string>()
                : e.is_string() ? e.get<std::string>() : safeDump(e);
        }
        *error = "the model answered HTTP " + std::to_string(status) + ": " + msg;
        return false;
    }
    try {
        const Json& c = r.at("choices").at(0).at("message").at("content");
        if (!c.is_string()) throw std::runtime_error("no text");
        *content = c.get<std::string>();
        if (stats) {
            // llama-server: "timings" (prompt/predicted counts, ms, tokens per
            // second). Other OpenAI-compatible servers: "usage" counts only.
            Json s = Json::object();
            auto num = [](const Json& o, const char* k) { return o.contains(k) && o[k].is_number() ? o[k].get<double>() : -1.0; };
            if (r.contains("timings") && r["timings"].is_object()) {
                const Json& t = r["timings"];
                s["promptTokens"] = num(t, "prompt_n");
                s["promptMs"] = num(t, "prompt_ms");
                s["genTokens"] = num(t, "predicted_n");
                s["genMs"] = num(t, "predicted_ms");
            } else if (r.contains("usage") && r["usage"].is_object()) {
                s["promptTokens"] = num(r["usage"], "prompt_tokens");
                s["genTokens"] = num(r["usage"], "completion_tokens");
            }
            *stats = s;
        }
    } catch (const std::exception&) {
        *error = "the model's answer has no message";
        return false;
    }
    return true;
}
