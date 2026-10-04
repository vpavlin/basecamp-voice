#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

// What Basecamp Voice needs on disk, fetched once and only after the user
// confirms (sizes shown first): the llama.cpp server for this platform, the
// language model and the speech model. Everything is pinned and checked by
// sha256; interrupted downloads resume. Kept in the module's data directory.
struct AssetSpec {
    std::string id;          // "runtime" | "llm" | "stt"
    std::string title;
    std::string url;
    std::string sha256;
    long long size = 0;
    std::string file;        // name in the data directory
    bool archive = false;    // a .tar.gz to unpack (the runtime)
    std::string entry;       // for archives: the path inside to the program
    std::string choice;      // for "llm": which model this is ("qwen3-4b", ...)
};

class Assets {
public:
    // Downloads the pinned specs; tests pass their own.
    explicit Assets(std::string dir, std::vector<AssetSpec> specs = pinned());
    ~Assets();

    static std::vector<AssetSpec> pinned();
    static bool vulkanAvailable();
    // The language models a user can choose from: [{id, title, size}].
    nlohmann::json llmChoices() const;
    std::string llmChoice() const;
    // Fails while downloading, or for an unknown id.
    bool setLlmChoice(const std::string& choice, std::string* error);

    // {state: missing|downloading|ready|failed, error, items: [{id,title,size,have,received}]}
    nlohmann::json status() const;
    bool have(const std::string& id) const;
    // The file to use: the model file, or the runtime's program.
    std::string path(const std::string& id) const;

    // Fetches what is missing, in the background. `only` limits it to some ids.
    // False (with a reason) while an earlier download is still stopping.
    bool start(const std::vector<std::string>& only = {}, std::string* error = nullptr);
    void cancel();

    // Only for tests: how files are fetched (url, dest, size, sha, progress, error).
    using Fetch = std::function<bool(const AssetSpec&, const std::string& dest,
                                     const std::function<bool(long long, long long)>& progress, std::string* error)>;
    Fetch fetch;

private:
    struct Item {
        AssetSpec spec;
        bool have = false;
        long long received = 0;
    };
    void work(std::vector<std::string> only);
    bool verifyExisting(Item& item);
    bool selected(const Item& item) const;   // m_mu held
    bool install(Item& item, std::string* error);
    std::string marker(const AssetSpec& spec) const;

    std::string m_dir;
    mutable std::mutex m_mu;
    std::vector<Item> m_items;
    std::string m_state = "missing";
    std::string m_llmChoice = "qwen3-4b";
    std::string m_error;
    std::atomic<bool> m_cancel{false};
    std::thread m_worker;
};
