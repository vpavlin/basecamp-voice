#include "assets.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sys/stat.h>

#include <dlfcn.h>
#include <dirent.h>

#include "net.h"
#include "proc.h"

using Json = nlohmann::json;

namespace {

bool exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

long long sizeOf(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 ? static_cast<long long>(st.st_size) : -1;
}

std::string readAll(const std::string& p) {
    std::ifstream f(p);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s;
}

void writeAll(const std::string& p, const std::string& s) {
    std::ofstream f(p, std::ios::trunc);
    f << s;
}

const char* kLlamaTag = "b11379";

bool hasDriverManifest(const char* dir) {
    DIR* d = ::opendir(dir);
    if (!d) return false;
    bool found = false;
    while (dirent* e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0) { found = true; break; }
    }
    ::closedir(d);
    return found;
}

}  // namespace

// A Vulkan loader and at least one driver: the iGPU then reads prompts about
// four times faster than the CPU (docs/adr/0007).
bool Assets::vulkanAvailable() {
#if defined(__linux__)
    void* h = ::dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!h) return false;
    ::dlclose(h);
    for (const char* dir : {"/usr/share/vulkan/icd.d", "/etc/vulkan/icd.d", "/usr/local/share/vulkan/icd.d",
                            "/run/opengl-driver/share/vulkan/icd.d"})
        if (hasDriverManifest(dir)) return true;
#endif
    return false;
}

namespace {

}  // namespace

std::vector<AssetSpec> Assets::pinned() {
    std::vector<AssetSpec> specs;
    // llama.cpp's prebuilt server, CPU build (docs/adr/0007).
    const std::string base = std::string("https://github.com/ggml-org/llama.cpp/releases/download/") + kLlamaTag + "/";
    AssetSpec rt;
    rt.id = "runtime";
    rt.title = "llama.cpp server";
    rt.archive = true;
    rt.entry = std::string("llama-") + kLlamaTag + "/llama-server";
#if defined(__linux__) && defined(__x86_64__)
    if (vulkanAvailable()) {
        rt.title = "llama.cpp server (Vulkan)";
        rt.file = std::string("llama-") + kLlamaTag + "-bin-ubuntu-vulkan-x64.tar.gz";
        rt.sha256 = "8e2e41e3135b7879039aa81776246c7263e57cedaab071daa70bc294a807250b";
        rt.size = 31603456;
    } else {
        rt.file = std::string("llama-") + kLlamaTag + "-bin-ubuntu-x64.tar.gz";
        rt.sha256 = "8ab0e8588e2b282ed4882a47a26dbf1a2ae920578deb24a8dfe390110c1bb924";
        rt.size = 17658949;
    }
#elif defined(__linux__) && defined(__aarch64__)
    rt.file = std::string("llama-") + kLlamaTag + "-bin-ubuntu-arm64.tar.gz";
    rt.sha256 = "6085fcde1c11a7503cc0d7843428c72bc0ae3f829a86ec368a37fd57555ca9e8";
    rt.size = 13688103;
#elif defined(__APPLE__) && defined(__aarch64__)
    rt.file = std::string("llama-") + kLlamaTag + "-bin-macos-arm64.tar.gz";
    rt.sha256 = "1b04dbf9b48045b377458c49daced5076aa8d03f23384b6ac4e48db797492f06";
    rt.size = 11917888;
#endif
    if (!rt.file.empty()) {
        rt.url = base + rt.file;
        specs.push_back(rt);
    }

    // Two pinned choices (docs/model-eval.md): careful (the default) and fast.
    AssetSpec llm;
    llm.id = "llm";
    llm.choice = "qwen3-4b";
    llm.title = "Language model: careful (Qwen3 4B Instruct 2507)";
    llm.url = "https://huggingface.co/unsloth/Qwen3-4B-Instruct-2507-GGUF/resolve/"
              "a06e946bb6b655725eafa393f4a9745d460374c9/Qwen3-4B-Instruct-2507-Q4_K_M.gguf";
    llm.sha256 = "3605803b982cb64aead44f6c1b2ae36e3acdb41d8e46c8a94c6533bc4c67e597";
    llm.size = 2497281120LL;
    llm.file = "Qwen3-4B-Instruct-2507-Q4_K_M.gguf";
    specs.push_back(llm);

    AssetSpec fast;
    fast.id = "llm";
    fast.choice = "qwen3.5-2b";
    fast.title = "Language model: fast (Qwen3.5 2B)";
    fast.url = "https://huggingface.co/unsloth/Qwen3.5-2B-GGUF/resolve/"
               "f6d5376be1edb4d416d56da11e5397a961aca8ae/Qwen3.5-2B-Q4_K_M.gguf";
    fast.sha256 = "aaf42c8b7c3cab2bf3d69c355048d4a0ee9973d48f16c731c0520ee914699223";
    fast.size = 1280835840LL;
    fast.file = "Qwen3.5-2B-Q4_K_M.gguf";
    specs.push_back(fast);

    AssetSpec stt;
    stt.id = "stt";
    stt.title = "Speech model (Parakeet TDT 0.6B v3)";
    stt.url = "https://huggingface.co/ggml-org/parakeet-GGUF/resolve/"
              "35156454d1a39de06863303dd209fd2bed6ee079/ggml-parakeet-tdt-0.6b-v3-q4_k.bin";
    stt.sha256 = "8b205b8b39c6535e153de6fb11c51db46125d45c4f16ba496fe41a0fe71b885e";
    stt.size = 415611879LL;
    stt.file = "ggml-parakeet-tdt-0.6b-v3-q4_k.bin";
    specs.push_back(stt);
    return specs;
}

Assets::Assets(std::string dir, std::vector<AssetSpec> specs) : m_dir(std::move(dir)) {
    ::mkdir(m_dir.c_str(), 0755);
    fetch = [](const AssetSpec& s, const std::string& dest,
               const std::function<bool(long long, long long)>& progress, std::string* error) {
        return net::download(s.url, dest, s.size, s.sha256, progress, error);
    };
    for (auto& s : specs) {
        Item item;
        item.spec = s;
        item.have = exists(marker(s)) && readAll(marker(s)) == s.sha256;
        m_items.push_back(item);
    }
    // A marker says the file was verified; the file itself must still be there.
    for (auto& item : m_items) {
        if (!item.have) continue;
        const std::string p = item.spec.archive ? m_dir + "/runtime/" + item.spec.entry : m_dir + "/" + item.spec.file;
        if (!exists(p)) item.have = false;
    }
    bool all = true;
    for (const auto& i : m_items) if (selected(i)) all = all && i.have;
    m_state = all ? "ready" : "missing";
}

Assets::~Assets() {
    cancel();
    if (m_worker.joinable()) m_worker.join();
}

std::string Assets::marker(const AssetSpec& spec) const {
    return m_dir + "/." + spec.id + (spec.choice.empty() ? "" : "-" + spec.choice) + ".verified";
}

bool Assets::selected(const Item& item) const {
    return item.spec.id != "llm" || item.spec.choice == m_llmChoice;
}

Json Assets::llmChoices() const {
    Json out = Json::array();
    for (const auto& i : m_items)
        if (i.spec.id == "llm") out.push_back({{"id", i.spec.choice}, {"title", i.spec.title}, {"size", i.spec.size}});
    return out;
}

std::string Assets::llmChoice() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_llmChoice;
}

bool Assets::setLlmChoice(const std::string& choice, std::string* error) {
    std::lock_guard<std::mutex> lk(m_mu);
    if (m_state == "downloading") { *error = "Wait for the download to finish (or stop it) first."; return false; }
    bool known = false;
    for (const auto& i : m_items) known = known || (i.spec.id == "llm" && i.spec.choice == choice);
    if (!known) { *error = "Unknown model \"" + choice + "\"."; return false; }
    m_llmChoice = choice;
    bool all = true;
    for (const auto& i : m_items) if (selected(i)) all = all && i.have;
    m_state = all ? "ready" : "missing";
    m_error.clear();
    return true;
}

Json Assets::status() const {
    std::lock_guard<std::mutex> lk(m_mu);
    Json items = Json::array();
    long long missing = 0;
    for (const auto& i : m_items) {
        if (!selected(i)) continue;
        items.push_back({{"id", i.spec.id}, {"title", i.spec.title}, {"size", i.spec.size},
                         {"have", i.have}, {"received", i.received}});
        if (!i.have) missing += i.spec.size;
    }
    return {{"state", m_state}, {"error", m_error}, {"items", items}, {"missingBytes", missing},
            {"llm", m_llmChoice}};
}

bool Assets::have(const std::string& id) const {
    std::lock_guard<std::mutex> lk(m_mu);
    for (const auto& i : m_items) if (i.spec.id == id && selected(i)) return i.have;
    return false;
}

std::string Assets::path(const std::string& id) const {
    std::lock_guard<std::mutex> lk(m_mu);
    for (const auto& i : m_items) {
        if (i.spec.id != id || !selected(i)) continue;
        return i.spec.archive ? m_dir + "/runtime/" + i.spec.entry : m_dir + "/" + i.spec.file;
    }
    return {};
}

bool Assets::start(const std::vector<std::string>& only, std::string* error) {
    std::lock_guard<std::mutex> lk(m_mu);
    if (m_state == "downloading") {
        if (!m_cancel) return true;   // already on its way
        if (error) *error = "The last download is still stopping; try again in a moment.";
        return false;
    }
    if (m_worker.joinable()) m_worker.join();
    m_cancel = false;
    m_state = "downloading";
    m_error.clear();
    m_worker = std::thread([this, only]() {
        try {
            work(only);
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> g(m_mu);
            m_state = "failed";
            m_error = e.what();
        }
    });
    return true;
}

void Assets::cancel() { m_cancel = true; }

bool Assets::verifyExisting(Item& item) {
    // A file already in place (an earlier run, or copied in): accept it once
    // its size and checksum match, without downloading again.
    const std::string dest = m_dir + "/" + item.spec.file;
    if (sizeOf(dest) != item.spec.size) return false;
    std::string err;
    return net::sha256File(dest, &err) == item.spec.sha256;
}

bool Assets::install(Item& item, std::string* error) {
    const std::string dest = m_dir + "/" + item.spec.file;
    if (!verifyExisting(item)) {
        auto progress = [this, &item](long long got, long long) {
            std::lock_guard<std::mutex> lk(m_mu);
            item.received = got;
            return !m_cancel.load();
        };
        if (!fetch(item.spec, dest, progress, error)) return false;
    }
    if (item.spec.archive) {
        const std::string out = m_dir + "/runtime";
        ::mkdir(out.c_str(), 0755);
        std::string err;
        const int rc = proc::run({"tar", "-xzf", dest, "-C", out}, "", 120000, &err);
        if (rc != 0) { *error = "could not unpack " + item.spec.file + (err.empty() ? "" : ": " + err); return false; }
        if (!exists(out + "/" + item.spec.entry)) { *error = item.spec.entry + " is not in " + item.spec.file; return false; }
        std::remove(dest.c_str());   // the unpacked copy is what is used
    }
    writeAll(marker(item.spec), item.spec.sha256);
    return true;
}

void Assets::work(std::vector<std::string> only) {
    for (auto& item : m_items) {
        {
            std::lock_guard<std::mutex> lk(m_mu);
            if (item.have || !selected(item)) continue;
            if (!only.empty() && std::find(only.begin(), only.end(), item.spec.id) == only.end()) continue;
        }
        std::string error;
        const bool ok = install(item, &error);
        std::lock_guard<std::mutex> lk(m_mu);
        if (!ok) {
            m_state = m_cancel ? "missing" : "failed";
            m_error = m_cancel ? "" : item.spec.title + ": " + error;
            return;
        }
        item.have = true;
        item.received = item.spec.size;
    }
    std::lock_guard<std::mutex> lk(m_mu);
    bool all = true;
    for (const auto& i : m_items) if (selected(i)) all = all && i.have;
    m_state = all ? "ready" : "missing";
}
