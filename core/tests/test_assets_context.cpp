// Assets (with a fake fetch) and the planner's context.

#include <logos_test.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>
#include <unistd.h>

#include "assets.h"
#include "fake_basecamp.h"
#include "net.h"
#include "tools.h"

namespace {

std::string tmpDir(const std::string& name) {
    const std::string d = "/tmp/basecamp_voice_test_" + name + "_" + std::to_string(::getpid());
    std::string cmd = "rm -rf '" + d + "' && mkdir -p '" + d + "'";
    std::system(cmd.c_str());
    return d;
}

std::vector<AssetSpec> specs() {
    AssetSpec careful{"llm", "careful", "u", "", 10, "careful.gguf", false, "", "qwen3-4b"};
    AssetSpec fast{"llm", "fast", "u", "", 5, "fast.gguf", false, "", "qwen3.5-2b"};
    AssetSpec stt{"stt", "speech", "u", "", 3, "stt.bin", false, "", ""};
    return {careful, fast, stt};
}

Json waitDone(Assets& a) {
    for (int i = 0; i < 300; ++i) {
        Json s = a.status();
        if (s["state"] != "downloading") return s;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return a.status();
}

}  // namespace

LOGOS_TEST(assets_show_and_fetch_only_the_chosen_model) {
    Assets a(tmpDir("choice"), specs());
    std::vector<std::string> fetched;
    a.fetch = [&](const AssetSpec& s, const std::string& dest, const std::function<bool(long long, long long)>&, std::string*) {
        fetched.push_back(s.file);
        std::ofstream(dest) << std::string(s.size, 'x');
        return true;
    };
    Json st = a.status();
    LOGOS_ASSERT_EQ(st["items"].size(), size_t(2));
    LOGOS_ASSERT_EQ(st["missingBytes"].get<long long>(), 13LL);
    std::string err;
    LOGOS_ASSERT_TRUE(a.setLlmChoice("qwen3.5-2b", &err));
    LOGOS_ASSERT_EQ(a.status()["missingBytes"].get<long long>(), 8LL);
    a.start();
    st = waitDone(a);
    LOGOS_ASSERT_EQ(st["state"].get<std::string>(), std::string("ready"));
    LOGOS_ASSERT_EQ(fetched.size(), size_t(2));
    LOGOS_ASSERT_TRUE(std::find(fetched.begin(), fetched.end(), "careful.gguf") == fetched.end());
    LOGOS_ASSERT_CONTAINS(a.path("llm"), std::string("fast.gguf"));
    // Back to the careful model: not downloaded, so not ready.
    LOGOS_ASSERT_TRUE(a.setLlmChoice("qwen3-4b", &err));
    LOGOS_ASSERT_FALSE(a.have("llm"));
    LOGOS_ASSERT_EQ(a.status()["state"].get<std::string>(), std::string("missing"));
    LOGOS_ASSERT_FALSE(a.setLlmChoice("gpt-17", &err));
}

LOGOS_TEST(assets_report_a_failed_download_and_refuse_to_switch_mid_download) {
    Assets a(tmpDir("fail"), specs());
    std::atomic<bool> release{false};
    a.fetch = [&](const AssetSpec& s, const std::string&, const std::function<bool(long long, long long)>&, std::string* error) {
        while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        *error = "checksum mismatch";
        return s.id != "llm";
    };
    a.start();
    std::string err;
    LOGOS_ASSERT_FALSE(a.setLlmChoice("qwen3.5-2b", &err));
    LOGOS_ASSERT_CONTAINS(err, std::string("download"));
    release = true;
    Json st = waitDone(a);
    LOGOS_ASSERT_EQ(st["state"].get<std::string>(), std::string("failed"));
    LOGOS_ASSERT_CONTAINS(st["error"].get<std::string>(), std::string("checksum mismatch"));
}

LOGOS_TEST(assets_unpack_the_runtime_archive) {
    const std::string dir = tmpDir("runtime");
    const std::string src = tmpDir("runtime_src");
    std::system(("mkdir -p '" + src + "/llama-x' && printf '#!/bin/sh\\n' > '" + src + "/llama-x/llama-server' && "
                 "tar -czf '" + src + "/rt.tar.gz' -C '" + src + "' llama-x").c_str());
    AssetSpec rt{"runtime", "server", "u", "", 0, "rt.tar.gz", true, "llama-x/llama-server", ""};
    Assets a(dir, {rt});
    a.fetch = [&](const AssetSpec&, const std::string& dest, const std::function<bool(long long, long long)>&, std::string*) {
        return std::system(("cp '" + src + "/rt.tar.gz' '" + dest + "'").c_str()) == 0;
    };
    a.start();
    Json st = waitDone(a);
    LOGOS_ASSERT_EQ(st["state"].get<std::string>(), std::string("ready"));
    LOGOS_ASSERT_EQ(::access(a.path("runtime").c_str(), F_OK), 0);
    // Verified once: a new Assets on the same directory sees it as present.
    Assets again(dir, {rt});
    LOGOS_ASSERT_TRUE(again.have("runtime"));
}

LOGOS_TEST(context_lists_what_matters_and_leaves_out_noise) {
    FakeBasecamp bc;
    bc.installed.insert("eth_rpc_ui");
    bc.installed.insert("eth_rpc_module");
    bc.ready.insert("eth_rpc_module");
    bc.methods["eth_rpc_module"] = Json::array({{{"name", "list_chains"}, {"parameters", Json::array()}},
                                                {{"name", "get_chain_config"}, {"parameters", {{{"name", "chain_id"}, {"type", "int"}}}}}});
    Tools t(bc, [](const std::string&) { return CallResult(); });
    const std::string c = t.context("please open the token list app");
    LOGOS_ASSERT_CONTAINS(c, std::string("Installed apps: eth_rpc_ui (needs eth_rpc_module)"));
    LOGOS_ASSERT_CONTAINS(c, std::string("Running modules: eth_rpc_module\n"));
    LOGOS_ASSERT_CONTAINS(c, std::string("Methods of eth_rpc_module: list_chains(), get_chain_config(chain_id: int)"));
    LOGOS_ASSERT_CONTAINS(c, std::string("- token_list_ui (app) needs token_list_module"));
    LOGOS_ASSERT_CONTAINS(c, std::string("User: \"please open the token list app\""));
    // Basecamp's own modules are not the user's business; filler words match nothing.
    LOGOS_ASSERT_TRUE(c.find("capability_module") == std::string::npos);
    LOGOS_ASSERT_TRUE(c.find("- eth_rpc_ui") == std::string::npos);
    LOGOS_ASSERT_TRUE(c.find("- blockchain") == std::string::npos);
}
