// RulePlanner: the stand-in phrasings until the model planner lands.

#include <logos_test.h>

#include "planner.h"

using Json = nlohmann::json;

namespace {
Json first(const std::string& text) {
    Json p = RulePlanner().plan(text);
    return p.value("ok", false) ? p["steps"][0] : p;
}
}

LOGOS_TEST(planner_maps_the_basic_phrasings) {
    LOGOS_ASSERT_EQ(first("Open eth rpc.")["tool"].get<std::string>(), std::string("open_app"));
    LOGOS_ASSERT_EQ(first("Open eth rpc.")["args"]["app"].get<std::string>(), std::string("eth rpc"));
    LOGOS_ASSERT_EQ(first("install and run the blockchain module")["tool"].get<std::string>(), std::string("open_app"));
    LOGOS_ASSERT_EQ(first("please install token list module")["args"]["name"].get<std::string>(), std::string("token list module"));
    LOGOS_ASSERT_EQ(first("what's installed?")["tool"].get<std::string>(), std::string("list_installed"));
    LOGOS_ASSERT_EQ(first("status")["tool"].get<std::string>(), std::string("status"));
    LOGOS_ASSERT_EQ(first("search wallet")["args"]["query"].get<std::string>(), std::string("wallet"));
    LOGOS_ASSERT_EQ(first("methods of eth_rpc_module")["args"]["module"].get<std::string>(), std::string("eth_rpc_module"));
}

LOGOS_TEST(planner_parses_calls_with_typed_arguments) {
    Json s = first("call eth_rpc_module.set_chain_config(1, {\"rpc\": \"https://a, b\"})");
    LOGOS_ASSERT_EQ(s["tool"].get<std::string>(), std::string("call"));
    LOGOS_ASSERT_EQ(s["args"]["method"].get<std::string>(), std::string("set_chain_config"));
    LOGOS_ASSERT_EQ(s["args"]["args"].size(), size_t(2));
    LOGOS_ASSERT_EQ(s["args"]["args"][0].get<int>(), 1);
    LOGOS_ASSERT_EQ(s["args"]["args"][1]["rpc"].get<std::string>(), std::string("https://a, b"));
    Json t = first("call eth_rpc_module.get_chain_config 1");
    LOGOS_ASSERT_EQ(t["args"]["args"][0].get<int>(), 1);
}

LOGOS_TEST(planner_chains_with_then_and_refuses_what_it_does_not_know) {
    Json p = RulePlanner().plan("open eth rpc then call eth_rpc_module.list_chains");
    LOGOS_ASSERT_TRUE(p["ok"].get<bool>());
    LOGOS_ASSERT_EQ(p["steps"].size(), size_t(2));
    Json bad = RulePlanner().plan("make me a sandwich");
    LOGOS_ASSERT_FALSE(bad["ok"].get<bool>());
    LOGOS_ASSERT_CONTAINS(bad["error"].get<std::string>(), std::string("can't plan"));
}
