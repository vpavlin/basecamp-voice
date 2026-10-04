// Planning in rounds: open an app, then plan with its methods in view.

#include <logos_test.h>

#include <chrono>
#include <thread>

#include "engine.h"
#include "fake_basecamp.h"

namespace {

struct ScriptedPlanner : Planner {
    std::vector<Json> rounds;          // answers, in order
    std::vector<Json> doneSeen;        // what planNext was given
    size_t next = 0;
    Json plan(const std::string&) override { return rounds.at(next++); }
    Json planNext(const std::string&, const Json& done) override {
        doneSeen.push_back(done);
        return next < rounds.size() ? rounds.at(next++) : Json{{"ok", true}, {"steps", Json::array()}, {"reply", "All done."}};
    }
    bool continues() const override { return true; }
};

Json step(const std::string& tool, Json args) { return {{"tool", tool}, {"args", std::move(args)}}; }

Json waitState(Engine& e, const std::string& id, const std::vector<std::string>& states, FakeBasecamp& bc) {
    Json job;
    for (int i = 0; i < 400; ++i) {
        const Json ev = e.events(0);
        if (ev.contains("viewAction")) {
            const std::string app = ev["viewAction"]["params"]["app"];
            {
                std::lock_guard<std::mutex> lk(bc.mu);
                for (const auto& d : bc.catalog[app].deps) bc.ready.insert(d);
            }
            e.viewActionDone(ev["viewAction"]["id"], "{\"ok\":true}");
        }
        for (const auto& j : ev["jobs"]) if (j["id"] == id) job = j;
        for (const auto& s : states) if (!job.is_null() && job["state"] == s) return job;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return job;
}

}  // namespace

LOGOS_TEST(rounds_open_the_app_then_plan_the_call_with_its_methods) {
    FakeBasecamp bc;
    bc.installed.insert("eth_rpc_ui");
    bc.installed.insert("eth_rpc_module");
    bc.methods["eth_rpc_module"] = Json::array({{{"name", "list_chains"}, {"parameters", Json::array()}}});
    bc.handlers["eth_rpc_module.list_chains"] = [](const Json&) { return Json("{\"chains\":[],\"ok\":true}"); };
    ScriptedPlanner planner;
    planner.rounds = {
        {{"ok", true}, {"reply", "Opening eth_rpc_ui first."}, {"steps", Json::array({step("open_app", {{"app", "eth_rpc_ui"}}),
                                                                                       step("open_app", {{"app", "eth_rpc_ui"}})})}},
        {{"ok", true}, {"reply", "Listing the chains."}, {"steps", Json::array({step("call", {{"module", "eth_rpc_module"}, {"method", "list_chains"}, {"args", Json::array()}})})}},
    };
    Engine e(bc, planner);
    e.tools().readyWaitMs = 2000;
    e.tools().readyPollMs = 10;
    const std::string id = e.submit("list the chains in eth rpc")["job"];

    Json job = waitState(e, id, {"awaiting_confirmation", "failed", "done"}, bc);
    LOGOS_ASSERT_EQ(job["state"].get<std::string>(), std::string("awaiting_confirmation"));
    LOGOS_ASSERT_EQ(job["steps"].size(), size_t(1));          // the repeated open_app was dropped
    e.confirm(id);

    // Round two: planned with what was done, and the call needs confirming too.
    job = waitState(e, id, {"awaiting_confirmation", "failed", "done"}, bc);
    while (job["state"] == "awaiting_confirmation" && job["steps"].size() < 2)
        job = waitState(e, id, {"awaiting_confirmation", "failed", "done"}, bc);
    LOGOS_ASSERT_EQ(job["state"].get<std::string>(), std::string("awaiting_confirmation"));
    LOGOS_ASSERT_EQ(job["steps"].size(), size_t(2));
    LOGOS_ASSERT_EQ(job["steps"][0]["status"].get<std::string>(), std::string("done"));
    LOGOS_ASSERT_EQ(planner.doneSeen.size(), size_t(1));
    LOGOS_ASSERT_EQ(planner.doneSeen[0][0]["step"].get<std::string>(), std::string("Open eth_rpc_ui"));
    LOGOS_ASSERT_CONTAINS(planner.doneSeen[0][0]["result"].get<std::string>(), std::string("eth_rpc_module is running"));
    e.confirm(id);

    job = waitState(e, id, {"failed", "done"}, bc);
    LOGOS_ASSERT_EQ(job["state"].get<std::string>(), std::string("done"));
    LOGOS_ASSERT_CONTAINS(job["summary"].get<std::string>(), std::string("Opened eth_rpc_ui."));
    LOGOS_ASSERT_CONTAINS(job["summary"].get<std::string>(), std::string("list_chains returned"));
    // A round that only called (no app opened, no methods read) is not followed up.
    LOGOS_ASSERT_EQ(planner.doneSeen.size(), size_t(1));
}

LOGOS_TEST(a_last_round_with_nothing_to_do_ends_with_its_reply) {
    FakeBasecamp bc;
    bc.installed.insert("blockchain_ui");
    bc.installed.insert("blockchain_module");
    ScriptedPlanner planner;
    planner.rounds = {
        {{"ok", true}, {"reply", "Opening blockchain_ui first."}, {"steps", Json::array({step("open_app", {{"app", "blockchain_ui"}})})}},
        {{"ok", true}, {"reply", "To start the node I need a configuration path. Which one?"}, {"steps", Json::array()},
         {"stats", {{"promptTokens", 300}, {"promptMs", 2000}, {"genTokens", 20}, {"genMs", 1500}}}},
    };
    planner.rounds[0]["stats"] = {{"promptTokens", 900}, {"promptMs", 6000}, {"genTokens", 40}, {"genMs", 3500}};
    Engine e(bc, planner);
    e.tools().readyWaitMs = 2000;
    e.tools().readyPollMs = 10;
    const std::string id = e.submit("start the blockchain node")["job"];
    Json job = waitState(e, id, {"awaiting_confirmation"}, bc);
    e.confirm(id);
    job = waitState(e, id, {"failed", "done"}, bc);
    LOGOS_ASSERT_EQ(job["state"].get<std::string>(), std::string("done"));
    LOGOS_ASSERT_EQ(job["summary"].get<std::string>(),
                    std::string("Opened blockchain_ui. blockchain_module is running. To start the node I need a configuration path. Which one?"));
    // The model's figures add up over both rounds.
    const Json& m = job["timing"]["model"];
    LOGOS_ASSERT_EQ(m["promptTokens"].get<int>(), 1200);
    LOGOS_ASSERT_EQ(m["genTokens"].get<int>(), 60);
    LOGOS_ASSERT_EQ(m["genMs"].get<int>(), 5000);
    LOGOS_ASSERT_EQ(m["requests"].get<int>(), 2);
}

LOGOS_TEST(a_follow_up_round_that_fails_keeps_the_job_done) {
    FakeBasecamp bc;
    ScriptedPlanner planner;
    planner.rounds = {
        {{"ok", true}, {"reply", "Listing."}, {"steps", Json::array({step("list_installed", Json::object())})}},
        {{"ok", false}, {"error", "the model answered HTTP 500: decode() failed: vk::Device::waitForFences: ErrorDeviceLost"}},
    };
    Engine e(bc, planner);
    const std::string id = e.submit("what is installed")["job"];
    Json job = waitState(e, id, {"failed", "done"}, bc);
    LOGOS_ASSERT_EQ(job["state"].get<std::string>(), std::string("done"));
    LOGOS_ASSERT_CONTAINS(job["summary"].get<std::string>(), std::string("Nothing is installed besides Basecamp itself."));
    LOGOS_ASSERT_CONTAINS(job["summary"].get<std::string>(), std::string("Could not plan anything further"));
}

LOGOS_TEST(a_conversation_remembers_finished_requests_and_their_lists) {
    FakeBasecamp bc;
    bc.installed.insert("eth_rpc_ui");
    ScriptedPlanner planner;
    planner.rounds = {
        {{"ok", true}, {"reply", "Listing."}, {"steps", Json::array({step("list_installed", Json::object())})}},
        {{"ok", true}, {"reply", "Those are installed."}, {"steps", Json::array()}},
    };
    Engine e(bc, planner);
    const std::string first = e.submit("what is installed")["job"];
    waitState(e, first, {"done", "failed"}, bc);
    Json h = e.history(6);
    LOGOS_ASSERT_EQ(h.size(), size_t(1));
    LOGOS_ASSERT_EQ(h[0]["said"].get<std::string>(), std::string("what is installed"));
    LOGOS_ASSERT_EQ(h[0]["lists"].get<std::string>(), std::string("Installed: 1. eth_rpc_ui (app 0.1.0)."));
    // A new conversation forgets it.
    e.newConversation();
    LOGOS_ASSERT_EQ(e.history(6).size(), size_t(0));
    LOGOS_ASSERT_EQ(e.events(0, false)["conversationStart"].get<int>(), 1);
}
