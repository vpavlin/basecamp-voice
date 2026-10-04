// Recipes: parsing, facts from Logos settings, and running an action.

#include <logos_test.h>

#include <cstdlib>
#include <fstream>
#include <unistd.h>

#include <chrono>
#include <thread>

#include "engine.h"
#include "fake_basecamp.h"
#include "model_planner.h"
#include "recipes.h"
#include "tools.h"

namespace {

std::string fakeHome(const std::string& name) {
    const std::string h = "/tmp/basecamp_voice_home_" + name + "_" + std::to_string(::getpid());
    std::system(("rm -rf '" + h + "' && mkdir -p '" + h + "/.config/Logos'").c_str());
    return h;
}

Tools toolsFor(FakeBasecamp& bc, const std::string& home, std::vector<std::string>* opened = nullptr) {
    Tools t(bc, [&bc, opened](const std::string& app) {
        if (opened) opened->push_back(app);
        std::lock_guard<std::mutex> lk(bc.mu);
        for (const auto& d : bc.catalog[app].deps) bc.ready.insert(d);
        CallResult r;
        r.ok = true;
        return r;
    });
    t.homeDir = home;
    t.readyWaitMs = 1000;
    t.readyPollMs = 10;
    return t;
}

}  // namespace

LOGOS_TEST(the_shipped_recipes_parse) {
    LOGOS_ASSERT_EQ(recipes::all().size(), size_t(2));
    const Recipe* b = recipes::forApp("blockchain_ui");
    LOGOS_ASSERT_TRUE(b != nullptr);
    LOGOS_ASSERT_EQ(b->module, std::string("blockchain_module"));
    LOGOS_ASSERT_TRUE(b->action("start_node") != nullptr);
    LOGOS_ASSERT_EQ(b->action("start_node")->steps.back().timeoutSec, 900);
    LOGOS_ASSERT_TRUE(recipes::forApp("storage_ui")->action("start_node") != nullptr);
}

LOGOS_TEST(a_malformed_recipe_is_refused_with_the_reason) {
    Recipe r;
    std::string err;
    LOGOS_ASSERT_FALSE(recipes::parse("app: a\nmodule: m\naction go: Go\n  call m.x($nope)\n", &r, &err));
    LOGOS_ASSERT_CONTAINS(err, std::string("$nope"));
    LOGOS_ASSERT_FALSE(recipes::parse("app: a\nmodule: m\naction go: Go\n  require missing: say\n", &r, &err));
    LOGOS_ASSERT_FALSE(recipes::parse("app: a\nmodule: m\naction go: Go\n  call m.x() later\n", &r, &err));
    LOGOS_ASSERT_FALSE(recipes::parse("module: m\n", &r, &err));
}

LOGOS_TEST(facts_come_only_from_logos_settings_and_paths_must_exist) {
    const std::string home = fakeHome("facts");
    std::ofstream(home + "/.config/Logos/BlockchainUI.conf")
        << "[General]\nuserConfigPath=" << home << "/user_config.yaml\ndeploymentConfigPath=\n";
    const Recipe* b = recipes::forApp("blockchain_ui");
    auto facts = recipes::resolveFacts(*b, home);
    LOGOS_ASSERT_FALSE(facts.count("config_path") > 0);   // the file is not there
    LOGOS_ASSERT_EQ(facts["deployment"], std::string(""));
    std::ofstream(home + "/user_config.yaml") << "x: 1\n";
    facts = recipes::resolveFacts(*b, home);
    LOGOS_ASSERT_EQ(facts["config_path"], home + "/user_config.yaml");
    Recipe evil;
    std::string err;
    LOGOS_ASSERT_TRUE(recipes::parse("app: a\nmodule: m\nfact k: setting ../../etc/passwd root\n", &evil, &err));
    LOGOS_ASSERT_EQ(recipes::resolveFacts(evil, home).size(), size_t(0));
}

LOGOS_TEST(the_plan_shows_the_calls_with_the_saved_values) {
    const std::string home = fakeHome("plan");
    std::ofstream(home + "/user_config.yaml") << "x: 1\n";
    std::ofstream(home + "/.config/Logos/BlockchainUI.conf") << "[General]\nuserConfigPath=" << home << "/user_config.yaml\n";
    FakeBasecamp bc;
    bc.installed.insert("blockchain_ui");
    bc.installed.insert("blockchain_module");
    Tools t = toolsFor(bc, home);
    Prepared p = t.prepare({{"tool", "recipe"}, {"args", {{"app", "blockchain"}, {"action", "start_node"}}}});
    LOGOS_ASSERT_TRUE(p.ok);
    LOGOS_ASSERT_TRUE(p.mutating);
    LOGOS_ASSERT_EQ(p.description, "Start the blockchain node: blockchain_module.start(" + home + "/user_config.yaml, \"\")");
}

LOGOS_TEST(a_recipe_that_needs_the_app_set_up_says_so) {
    const std::string home = fakeHome("notsetup");
    FakeBasecamp bc;
    bc.installed.insert("blockchain_ui");
    Tools t = toolsFor(bc, home);
    Prepared p = t.prepare({{"tool", "recipe"}, {"args", {{"app", "blockchain_ui"}, {"action", "start_node"}}}});
    LOGOS_ASSERT_FALSE(p.ok);
    LOGOS_ASSERT_CONTAINS(p.error, std::string("follow its setup"));
    const std::string c = t.context("start the blockchain node");
    LOGOS_ASSERT_CONTAINS(c, std::string("start_node (Start the blockchain node; not set up in the app yet)"));
}

LOGOS_TEST(storage_start_passes_the_loaded_config_and_opens_the_app_first) {
    const std::string home = fakeHome("storage");
    FakeBasecamp bc;
    bc.catalog["storage_ui"] = {"ui_qml", 1000, {"storage_module"}};
    bc.catalog["storage_module"] = {"core", 1000, {}};
    bc.installed.insert("storage_ui");
    bc.installed.insert("storage_module");
    std::vector<std::string> order;
    Json initArg;
    bc.methods["storage_module"] = Json::array();
    bc.handlers["storage_module.isRunning"] = [&](const Json&) { order.push_back("isRunning"); return Json(false); };
    bc.handlers["storage_module.loadConfigOrDefault"] = [&](const Json&) {
        order.push_back("load");
        return Json{{"success", true}, {"value", "{\"network\":\"logos.test\"}"}};
    };
    bc.handlers["storage_module.init"] = [&](const Json& a) { order.push_back("init"); initArg = a[0]; return Json(true); };
    bc.handlers["storage_module.start"] = [&](const Json&) { order.push_back("start"); return Json(true); };
    std::vector<std::string> opened;
    Tools t = toolsFor(bc, home, &opened);
    CallResult r = t.run({{"tool", "recipe"}, {"args", {{"app", "storage_ui"}, {"action", "start_node"}}}}, [](const std::string&) {});
    LOGOS_ASSERT_TRUE(r.ok);
    LOGOS_ASSERT_EQ(opened.size(), size_t(1));
    LOGOS_ASSERT_EQ(order.size(), size_t(4));
    LOGOS_ASSERT_EQ(order[1], std::string("load"));
    LOGOS_ASSERT_EQ(order[3], std::string("start"));
    LOGOS_ASSERT_EQ(initArg.get<std::string>(), std::string("{\"network\":\"logos.test\"}"));
}

LOGOS_TEST(storage_start_does_nothing_when_already_running) {
    FakeBasecamp bc;
    bc.catalog["storage_ui"] = {"ui_qml", 1000, {"storage_module"}};
    bc.catalog["storage_module"] = {"core", 1000, {}};
    bc.installed.insert("storage_ui");
    bc.ready.insert("storage_module");
    bool initCalled = false;
    bc.handlers["storage_module.isRunning"] = [](const Json&) { return Json(true); };
    bc.handlers["storage_module.init"] = [&](const Json&) { initCalled = true; return Json(true); };
    Tools t = toolsFor(bc, fakeHome("running"));
    CallResult r = t.run({{"tool", "recipe"}, {"args", {{"app", "storage_ui"}, {"action", "start_node"}}}}, [](const std::string&) {});
    LOGOS_ASSERT_TRUE(r.ok);
    LOGOS_ASSERT_EQ(r.value["summary"].get<std::string>(), std::string("The storage node is already running."));
    LOGOS_ASSERT_FALSE(initCalled);
}

LOGOS_TEST(the_schema_only_allows_recipes_that_exist) {
    const Json& any = ModelPlanner::schema()["properties"]["steps"]["items"]["anyOf"];
    Json recipeArgs;
    for (const auto& v : any) if (v["properties"]["tool"]["const"] == "recipe") recipeArgs = v["properties"]["args"];
    LOGOS_ASSERT_EQ(recipeArgs["anyOf"].size(), recipes::all().size());
    LOGOS_ASSERT_EQ(recipeArgs["anyOf"][0]["properties"]["app"]["const"].get<std::string>(), std::string("blockchain_ui"));
    LOGOS_ASSERT_TRUE(recipeArgs["anyOf"][0]["properties"]["action"]["enum"] == Json::array({"start_node", "stop_node"}));
}

namespace {
struct OnePlan : Planner {
    Json answer;
    Json plan(const std::string&) override { return answer; }
};
}

LOGOS_TEST(a_plan_that_acts_drops_its_redundant_checks) {
    FakeBasecamp bc;
    bc.installed.insert("eth_rpc_ui");
    OnePlan planner;
    planner.answer = {{"ok", true}, {"reply", "Checking, then opening."},
                      {"steps", Json::array({{{"tool", "list_installed"}, {"args", Json::object()}},
                                             {{"tool", "open_app"}, {"args", {{"app", "eth_rpc_ui"}}}}})}};
    Engine e(bc, planner);
    const std::string id = e.submit("check if eth rpc is installed and open it")["job"];
    Json job;
    for (int i = 0; i < 200; ++i) {
        const Json ev = e.events(0, false);
        job = ev["jobs"][0];
        if (job["state"] != "planning") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    LOGOS_ASSERT_EQ(job["steps"].size(), size_t(1));
    LOGOS_ASSERT_EQ(job["steps"][0]["description"].get<std::string>(), std::string("Open eth_rpc_ui"));
}

LOGOS_TEST(a_known_failure_becomes_advice) {
    const std::string home = fakeHome("stale");
    std::ofstream(home + "/user_config.yaml") << "x: 1\n";
    std::ofstream(home + "/.config/Logos/BlockchainUI.conf") << "[General]\nuserConfigPath=" << home << "/user_config.yaml\n";
    FakeBasecamp bc;
    bc.installed.insert("blockchain_ui");
    bc.installed.insert("blockchain_module");
    bc.ready.insert("blockchain_module");
    bc.handlers["blockchain_module.start"] = [](const Json&) {
        return Json{{"success", false}, {"error", "Could not parse config file: Unrecognized fields in value: [\"blend.core.backend.core_peering_degree\"]"}};
    };
    Tools t = toolsFor(bc, home);
    CallResult r = t.run({{"tool", "recipe"}, {"args", {{"app", "blockchain_ui"}, {"action", "start_node"}}}}, [](const std::string&) {});
    LOGOS_ASSERT_FALSE(r.ok);
    LOGOS_ASSERT_CONTAINS(r.error, std::string("use its Update config"));
    Recipe bad;
    std::string err;
    LOGOS_ASSERT_FALSE(recipes::parse("app: a\nmodule: m\naction go: Go\n  when_error no quotes: x\n", &bad, &err));
}
