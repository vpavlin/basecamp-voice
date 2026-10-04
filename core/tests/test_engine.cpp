// Engine: plan -> confirm -> run, cancel, and the open-app hand-off to the view.

#include <logos_test.h>

#include <chrono>
#include <thread>

#include "engine.h"
#include "fake_basecamp.h"

namespace {

Json jobOf(Engine& e, const std::string& id) {
    const Json ev = e.events(0);
    for (const auto& j : ev["jobs"]) if (j["id"] == id) return j;
    return nullptr;
}

// Poll like the view does; answer view actions the way Basecamp would.
Json waitFor(Engine& e, const std::string& id, const std::vector<std::string>& states,
             FakeBasecamp* bc = nullptr, bool answerViewActions = false) {
    for (int i = 0; i < 400; ++i) {
        Json ev = e.events(0);
        if (answerViewActions && ev.contains("viewAction")) {
            const std::string app = ev["viewAction"]["params"]["app"];
            {
                std::lock_guard<std::mutex> lk(bc->mu);
                for (const auto& d : bc->catalog[app].deps) bc->ready.insert(d);
            }
            e.viewActionDone(ev["viewAction"]["id"], "{\"ok\":true,\"error\":\"\"}");
        }
        Json j = jobOf(e, id);
        for (const auto& s : states) if (j["state"] == s) return j;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return jobOf(e, id);
}

}  // namespace

LOGOS_TEST(engine_runs_a_read_only_plan_without_asking) {
    FakeBasecamp bc;
    RulePlanner planner;
    Engine e(bc, planner);
    const std::string id = e.submit("what is installed")["job"];
    Json j = waitFor(e, id, {"done", "failed"});
    LOGOS_ASSERT_EQ(j["state"].get<std::string>(), std::string("done"));
    LOGOS_ASSERT_CONTAINS(j["summary"].get<std::string>(), std::string("Nothing is installed besides Basecamp itself."));
}

LOGOS_TEST(engine_waits_for_confirmation_before_changing_anything) {
    FakeBasecamp bc;
    RulePlanner planner;
    Engine e(bc, planner);
    const std::string id = e.submit("install eth rpc ui")["job"];
    Json j = waitFor(e, id, {"awaiting_confirmation", "failed"});
    LOGOS_ASSERT_EQ(j["state"].get<std::string>(), std::string("awaiting_confirmation"));
    LOGOS_ASSERT_TRUE(j["steps"][0]["mutating"].get<bool>());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    LOGOS_ASSERT_EQ(bc.count("package_manager.installPlugin"), 0);

    LOGOS_ASSERT_TRUE(e.confirm(id)["ok"].get<bool>());
    j = waitFor(e, id, {"done", "failed"});
    LOGOS_ASSERT_EQ(j["state"].get<std::string>(), std::string("done"));
    LOGOS_ASSERT_EQ(bc.installOrder.size(), size_t(2));
    LOGOS_ASSERT_FALSE(e.confirm(id)["ok"].get<bool>());   // not twice
}

LOGOS_TEST(engine_cancel_before_confirming_changes_nothing) {
    FakeBasecamp bc;
    RulePlanner planner;
    Engine e(bc, planner);
    const std::string id = e.submit("install eth rpc ui")["job"];
    waitFor(e, id, {"awaiting_confirmation"});
    LOGOS_ASSERT_TRUE(e.cancel(id)["ok"].get<bool>());
    LOGOS_ASSERT_FALSE(e.confirm(id)["ok"].get<bool>());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    LOGOS_ASSERT_EQ(jobOf(e, id)["state"].get<std::string>(), std::string("cancelled"));
    LOGOS_ASSERT_EQ(bc.count("package_manager.installPlugin"), 0);
}

LOGOS_TEST(engine_hands_open_app_to_the_view_and_finishes) {
    FakeBasecamp bc;
    RulePlanner planner;
    Engine e(bc, planner);
    e.tools().readyWaitMs = 2000;
    e.tools().readyPollMs = 10;
    const std::string id = e.submit("open eth rpc then call eth_rpc_module.list_chains")["job"];
    bc.methods["eth_rpc_module"] = Json::array({{{"name", "list_chains"}, {"parameters", Json::array()}}});
    bc.handlers["eth_rpc_module.list_chains"] = [](const Json&) { return Json("{\"chains\":[],\"ok\":true}"); };
    waitFor(e, id, {"awaiting_confirmation"});
    e.confirm(id);
    Json j = waitFor(e, id, {"done", "failed"}, &bc, true);
    LOGOS_ASSERT_EQ(j["state"].get<std::string>(), std::string("done"));
    LOGOS_ASSERT_EQ(j["steps"][0]["status"].get<std::string>(), std::string("done"));
    LOGOS_ASSERT_EQ(j["steps"][1]["status"].get<std::string>(), std::string("done"));
    LOGOS_ASSERT_CONTAINS(j["summary"].get<std::string>(), std::string("Opened eth_rpc_ui."));
}

LOGOS_TEST(engine_open_app_fails_clearly_when_no_view_is_polling) {
    FakeBasecamp bc;
    bc.installed.insert("eth_rpc_ui");
    bc.installed.insert("eth_rpc_module");
    RulePlanner planner;
    Engine e(bc, planner);
    e.viewWaitMs = 100;
    const std::string id = e.submit("open eth_rpc_ui")["job"];
    // Wait without calling events(): nobody picks up the view action.
    std::string state;
    for (int i = 0; i < 100 && state != "awaiting_confirmation"; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        state = e.events(1LL << 60)["jobs"][0]["state"];
    }
    e.confirm(id);
    Json j;
    for (int i = 0; i < 100; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        Json jobs = e.events(1LL << 60)["jobs"];   // since=huge: still hands out actions...
        j = jobs[0];
        if (j["state"] == "failed" || j["state"] == "done") break;
    }
    LOGOS_ASSERT_EQ(j["state"].get<std::string>(), std::string("failed"));
}

LOGOS_TEST(engine_only_the_views_poll_takes_a_view_action) {
    FakeBasecamp bc;
    bc.installed.insert("eth_rpc_ui");
    RulePlanner planner;
    Engine e(bc, planner);
    e.viewWaitMs = 2000;
    const std::string id = e.submit("open eth_rpc_ui")["job"];
    waitFor(e, id, {"awaiting_confirmation"});
    e.confirm(id);
    Json seen;
    for (int i = 0; i < 200 && seen.is_null(); ++i) {
        // A diagnostics read must not swallow the action meant for the view.
        LOGOS_ASSERT_FALSE(e.events(0, false).contains("viewAction"));
        Json ev = e.events(0);
        if (ev.contains("viewAction")) seen = ev["viewAction"];
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    LOGOS_ASSERT_EQ(seen["params"]["app"].get<std::string>(), std::string("eth_rpc_ui"));
    e.viewActionDone(seen["id"], "{\"ok\":true}");
}

LOGOS_TEST(engine_events_since_returns_only_newer_events) {
    FakeBasecamp bc;
    RulePlanner planner;
    Engine e(bc, planner);
    const std::string id = e.submit("status")["job"];
    waitFor(e, id, {"done"});
    Json all = e.events(0);
    const long long last = all["seq"].get<long long>();
    LOGOS_ASSERT_GT(all["events"].size(), size_t(3));
    LOGOS_ASSERT_EQ(e.events(last)["events"].size(), size_t(0));
    LOGOS_ASSERT_EQ(e.events(last - 1)["events"].size(), size_t(1));
}

LOGOS_TEST(engine_reports_a_plan_it_cannot_make) {
    FakeBasecamp bc;
    RulePlanner planner;
    Engine e(bc, planner);
    const std::string id = e.submit("open spreadsheet")["job"];
    Json j = waitFor(e, id, {"failed", "awaiting_confirmation"});
    LOGOS_ASSERT_EQ(j["state"].get<std::string>(), std::string("failed"));
    LOGOS_ASSERT_CONTAINS(j["summary"].get<std::string>(), std::string("No app called"));
}
