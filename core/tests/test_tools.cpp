// Tools: the checks every call goes through, name resolution, and the
// install / open-app sequences, against FakeBasecamp.

#include <logos_test.h>

#include "fake_basecamp.h"
#include "tools.h"

namespace {

Json ethRpcMethods() {
    return Json::array({
        {{"name", "list_chains"}, {"parameters", Json::array()}, {"returnType", "QString"}},
        {{"name", "get_chain_config"}, {"parameters", {{{"name", "chain_id"}, {"type", "int"}}}}, {"returnType", "QString"}},
        {{"name", "set_chain_config"}, {"parameters", {{{"name", "chain_id"}, {"type", "int"}}, {{"name", "config_json"}, {"type", "QString"}}}}, {"returnType", "bool"}},
    });
}

// eth_rpc installed and running, with methods that answer like the real one.
void runningEthRpc(FakeBasecamp& bc) {
    bc.installed.insert("eth_rpc_module");
    bc.installed.insert("eth_rpc_ui");
    bc.ready.insert("eth_rpc_module");
    bc.methods["eth_rpc_module"] = ethRpcMethods();
    bc.handlers["eth_rpc_module.list_chains"] = [](const Json&) { return Json("{\"chains\":[],\"ok\":true}"); };
    bc.handlers["eth_rpc_module.get_chain_config"] = [](const Json& a) {
        return Json(Json{{"ok", false}, {"error", "no config for chain " + (a.empty() ? std::string("?") : a[0].dump())}}.dump());
    };
}

Tools makeTools(FakeBasecamp& bc, std::vector<std::string>* opened = nullptr) {
    Tools t(bc, [&bc, opened](const std::string& app) {
        if (opened) opened->push_back(app);
        // Opening an app makes Basecamp load its cores.
        for (const auto& d : bc.catalog[app].deps) bc.ready.insert(d);
        CallResult r;
        r.ok = true;
        r.value = {{"ok", true}, {"error", ""}};
        return r;
    });
    t.readyWaitMs = 300;
    t.readyPollMs = 10;
    return t;
}

Json callStep(const std::string& module, const std::string& method, Json args) {
    return {{"tool", "call"}, {"args", {{"module", module}, {"method", method}, {"args", args}}}};
}

auto noProgress = [](const std::string&) {};

}  // namespace

// ---- helpers ------------------------------------------------------------------

LOGOS_TEST(unwrap_decodes_up_to_two_json_layers) {
    LOGOS_ASSERT_EQ(Tools::unwrap(Json("{\"a\":1}"))["a"].get<int>(), 1);
    const Json twice = Json(Json(Json{{"a", 2}}.dump()).dump());
    LOGOS_ASSERT_EQ(Tools::unwrap(twice)["a"].get<int>(), 2);
    LOGOS_ASSERT_EQ(Tools::unwrap(Json("plain words")).get<std::string>(), std::string("plain words"));
}

LOGOS_TEST(inner_failure_recognises_the_shapes_modules_use) {
    std::string err;
    LOGOS_ASSERT_TRUE(Tools::innerFailure({{"ok", false}, {"error", "nope"}}, &err));
    LOGOS_ASSERT_EQ(err, std::string("nope"));
    LOGOS_ASSERT_TRUE(Tools::innerFailure({{"success", false}}, &err));
    LOGOS_ASSERT_TRUE(Tools::innerFailure({{"error", "User modules directory is not set"}}, &err));
    LOGOS_ASSERT_FALSE(Tools::innerFailure({{"ok", true}, {"error", ""}}, &err));
    LOGOS_ASSERT_FALSE(Tools::innerFailure(Json::array(), &err));
}

LOGOS_TEST(read_only_methods_by_name) {
    for (const char* m : {"get_chain_config", "getPluginMethods", "list_chains", "is_ready", "isReady", "status", "list"})
        LOGOS_ASSERT_TRUE(Tools::isReadOnlyMethod(m));
    for (const char* m : {"set_chain_config", "start", "issue", "listen", "install", "getaway", "remove_chain_config"})
        LOGOS_ASSERT_FALSE(Tools::isReadOnlyMethod(m));
}

// ---- call: the checks ---------------------------------------------------------

LOGOS_TEST(call_is_refused_when_the_module_is_not_running) {
    FakeBasecamp bc;
    runningEthRpc(bc);
    bc.ready.erase("eth_rpc_module");
    Tools t = makeTools(bc);
    CallResult r = t.run(callStep("eth_rpc_module", "list_chains", Json::array()), noProgress);
    LOGOS_ASSERT_FALSE(r.ok);
    LOGOS_ASSERT_CONTAINS(r.error, std::string("not running"));
    // Never reached the target: an unloaded target would block for the whole timeout.
    LOGOS_ASSERT_EQ(bc.count("eth_rpc_module.list_chains"), 0);
    LOGOS_ASSERT_EQ(bc.count("eth_rpc_module.getPluginMethods"), 0);
}

LOGOS_TEST(call_is_refused_for_a_method_the_module_does_not_have) {
    FakeBasecamp bc;
    runningEthRpc(bc);
    Tools t = makeTools(bc);
    CallResult r = t.run(callStep("eth_rpc_module", "list_chainz", Json::array()), noProgress);
    LOGOS_ASSERT_FALSE(r.ok);
    LOGOS_ASSERT_CONTAINS(r.error, std::string("no method called list_chainz"));
    LOGOS_ASSERT_EQ(bc.count("eth_rpc_module.list_chainz"), 0);
}

LOGOS_TEST(call_is_refused_with_the_wrong_number_of_arguments) {
    FakeBasecamp bc;
    runningEthRpc(bc);
    Tools t = makeTools(bc);
    CallResult r = t.run(callStep("eth_rpc_module", "get_chain_config", Json::array()), noProgress);
    LOGOS_ASSERT_FALSE(r.ok);
    LOGOS_ASSERT_CONTAINS(r.error, std::string("takes 1 arguments, not 0"));
    LOGOS_ASSERT_EQ(bc.count("eth_rpc_module.get_chain_config"), 0);
}

LOGOS_TEST(call_reports_a_failure_inside_the_result_as_a_failure) {
    FakeBasecamp bc;
    runningEthRpc(bc);
    Tools t = makeTools(bc);
    CallResult r = t.run(callStep("eth_rpc_module", "get_chain_config", Json::array({1})), noProgress);
    LOGOS_ASSERT_FALSE(r.ok);
    LOGOS_ASSERT_CONTAINS(r.error, std::string("no config for chain 1"));
}

LOGOS_TEST(call_returns_the_decoded_result_and_a_summary) {
    FakeBasecamp bc;
    runningEthRpc(bc);
    Tools t = makeTools(bc);
    CallResult r = t.run(callStep("eth_rpc_module", "list_chains", Json::array()), noProgress);
    LOGOS_ASSERT_TRUE(r.ok);
    LOGOS_ASSERT_TRUE(r.value["result"]["chains"].is_array());
    LOGOS_ASSERT_CONTAINS(r.value["summary"].get<std::string>(), std::string("eth_rpc_module.list_chains returned"));
}

LOGOS_TEST(every_call_needs_confirmation_and_system_modules_are_off_limits) {
    FakeBasecamp bc;
    Tools t = makeTools(bc);
    // Names are no guide: "get_or_create_wallet" changes things.
    LOGOS_ASSERT_TRUE(t.prepare(callStep("m", "get_or_create_wallet", Json::array())).mutating);
    LOGOS_ASSERT_TRUE(t.prepare(callStep("m", "list_chains", Json::array())).mutating);
    LOGOS_ASSERT_FALSE(t.prepare(callStep("package_manager", "installPlugin", Json::array({"/tmp/x.lgx"}))).ok);
    LOGOS_ASSERT_FALSE(t.prepare(callStep("basecamp_voice_core", "configure", Json::array({"{}"}))).ok);
    LOGOS_ASSERT_FALSE(t.prepare({{"tool", "rm_rf"}, {"args", Json::object()}}).ok);
}

// ---- resolving names ----------------------------------------------------------

LOGOS_TEST(open_app_resolves_spoken_names_to_one_app) {
    FakeBasecamp bc;
    Tools t = makeTools(bc);
    auto app = [&](const std::string& q) {
        Prepared p = t.prepare({{"tool", "open_app"}, {"args", {{"app", q}}}});
        return p.ok ? p.step["args"]["app"].get<std::string>() : "ERROR: " + p.error;
    };
    LOGOS_ASSERT_EQ(app("eth rpc"), std::string("eth_rpc_ui"));
    LOGOS_ASSERT_EQ(app("the Eth RPC app"), std::string("eth_rpc_ui"));
    LOGOS_ASSERT_EQ(app("eth_rpc_ui"), std::string("eth_rpc_ui"));
    // A core's name finds the app that depends on it.
    LOGOS_ASSERT_EQ(app("blockchain module"), std::string("blockchain_ui"));
    LOGOS_ASSERT_EQ(app("token list"), std::string("token_list_ui"));
}

LOGOS_TEST(open_app_asks_when_a_name_is_ambiguous_or_unknown) {
    FakeBasecamp bc;
    Tools t = makeTools(bc);
    Prepared p = t.prepare({{"tool", "open_app"}, {"args", {{"app", "wallet"}}}});
    LOGOS_ASSERT_FALSE(p.ok);
    LOGOS_ASSERT_CONTAINS(p.error, std::string("lez_wallet_ui"));
    LOGOS_ASSERT_CONTAINS(p.error, std::string("monero_wallet_ui"));
    Prepared q = t.prepare({{"tool", "open_app"}, {"args", {{"app", "spreadsheet"}}}});
    LOGOS_ASSERT_FALSE(q.ok);
    LOGOS_ASSERT_CONTAINS(q.error, std::string("No app called"));
}

// ---- install --------------------------------------------------------------------

LOGOS_TEST(prepare_install_shows_dependencies_and_size_and_needs_confirmation) {
    FakeBasecamp bc;
    Tools t = makeTools(bc);
    Prepared p = t.prepare({{"tool", "install"}, {"args", {{"name", "eth_rpc_ui"}}}});
    LOGOS_ASSERT_TRUE(p.ok);
    LOGOS_ASSERT_TRUE(p.mutating);
    LOGOS_ASSERT_EQ(p.description, std::string("Install eth_rpc_ui 0.1.0 with eth_rpc_module (44 MB)"));
    // Preparing installs nothing.
    LOGOS_ASSERT_EQ(bc.count("package_manager.installPlugin"), 0);
    LOGOS_ASSERT_EQ(bc.count("package_downloader.downloadResolvedDependencies"), 0);
}

LOGOS_TEST(prepare_install_of_an_installed_package_changes_nothing) {
    FakeBasecamp bc;
    runningEthRpc(bc);
    Tools t = makeTools(bc);
    Prepared p = t.prepare({{"tool", "install"}, {"args", {{"name", "eth rpc module"}}}});
    LOGOS_ASSERT_TRUE(p.ok);
    LOGOS_ASSERT_FALSE(p.mutating);
    LOGOS_ASSERT_CONTAINS(p.description, std::string("already installed"));
}

LOGOS_TEST(install_installs_dependencies_first) {
    FakeBasecamp bc;
    Tools t = makeTools(bc);
    std::vector<std::string> progress;
    CallResult r = t.run({{"tool", "install"}, {"args", {{"name", "eth_rpc_ui"}}}},
                         [&](const std::string& m) { progress.push_back(m); });
    LOGOS_ASSERT_TRUE(r.ok);
    LOGOS_ASSERT_EQ(bc.installOrder.size(), size_t(2));
    LOGOS_ASSERT_EQ(bc.installOrder[0], std::string("eth_rpc_module"));
    LOGOS_ASSERT_EQ(bc.installOrder[1], std::string("eth_rpc_ui"));
    LOGOS_ASSERT_EQ(r.value["summary"].get<std::string>(), std::string("Installed eth_rpc_module and eth_rpc_ui."));
    LOGOS_ASSERT_GE(progress.size(), size_t(3));
    bool sawBytes = false;
    for (const auto& m : progress) sawBytes = sawBytes || m == "Downloading eth_rpc_ui: 14 MB of 14 MB";
    LOGOS_ASSERT_TRUE(sawBytes);
}

LOGOS_TEST(install_stops_at_a_failed_download_and_says_which) {
    FakeBasecamp bc;
    bc.downloadErrors["eth_rpc_module"] = "hash mismatch";
    Tools t = makeTools(bc);
    CallResult r = t.run({{"tool", "install"}, {"args", {{"name", "eth_rpc_ui"}}}}, noProgress);
    LOGOS_ASSERT_FALSE(r.ok);
    LOGOS_ASSERT_CONTAINS(r.error, std::string("eth_rpc_module: hash mismatch"));
    LOGOS_ASSERT_EQ(bc.installOrder.size(), size_t(0));
}

// ---- open_app -------------------------------------------------------------------

LOGOS_TEST(open_app_installs_opens_and_waits_for_the_core) {
    FakeBasecamp bc;
    std::vector<std::string> opened;
    Tools t = makeTools(bc, &opened);
    Prepared p = t.prepare({{"tool", "open_app"}, {"args", {{"app", "eth rpc"}}}});
    LOGOS_ASSERT_TRUE(p.ok);
    LOGOS_ASSERT_EQ(p.description, std::string("Install eth_rpc_ui with eth_rpc_module (44 MB), then open it"));
    CallResult r = t.run(p.step, noProgress);
    LOGOS_ASSERT_TRUE(r.ok);
    LOGOS_ASSERT_EQ(opened.size(), size_t(1));
    LOGOS_ASSERT_EQ(opened[0], std::string("eth_rpc_ui"));
    LOGOS_ASSERT_EQ(r.value["running"][0].get<std::string>(), std::string("eth_rpc_module"));
    LOGOS_ASSERT_EQ(r.value["summary"].get<std::string>(),
                    std::string("Installed eth_rpc_module and eth_rpc_ui. Opened eth_rpc_ui. eth_rpc_module is running."));
}

LOGOS_TEST(open_app_fails_when_the_core_never_starts) {
    FakeBasecamp bc;
    bc.installed.insert("eth_rpc_module");
    bc.installed.insert("eth_rpc_ui");
    Tools t(bc, [](const std::string&) {   // opens, but Basecamp loads nothing
        CallResult r;
        r.ok = true;
        return r;
    });
    t.readyWaitMs = 100;
    t.readyPollMs = 10;
    CallResult r = t.run({{"tool", "open_app"}, {"args", {{"app", "eth_rpc_ui"}, {"install", false}}}}, noProgress);
    LOGOS_ASSERT_FALSE(r.ok);
    LOGOS_ASSERT_CONTAINS(r.error, std::string("eth_rpc_module did not start"));
}

LOGOS_TEST(open_app_reports_when_basecamp_refuses) {
    FakeBasecamp bc;
    bc.installed.insert("eth_rpc_ui");
    Tools t(bc, [](const std::string&) {
        CallResult r;
        r.error = "Basecamp said not_declared.";
        return r;
    });
    CallResult r = t.run({{"tool", "open_app"}, {"args", {{"app", "eth_rpc_ui"}, {"install", false}}}}, noProgress);
    LOGOS_ASSERT_FALSE(r.ok);
    LOGOS_ASSERT_CONTAINS(r.error, std::string("not_declared"));
}

// ---- reading ----------------------------------------------------------------------

LOGOS_TEST(module_methods_lists_names_and_parameters) {
    FakeBasecamp bc;
    runningEthRpc(bc);
    Tools t = makeTools(bc);
    CallResult r = t.run({{"tool", "module_methods"}, {"args", {{"module", "eth_rpc_module"}}}}, noProgress);
    LOGOS_ASSERT_TRUE(r.ok);
    LOGOS_ASSERT_EQ(r.value["methods"][1]["name"].get<std::string>(), std::string("get_chain_config"));
    LOGOS_ASSERT_EQ(r.value["methods"][1]["parameters"][0].get<std::string>(), std::string("chain_id: int"));
}

LOGOS_TEST(list_available_filters_by_words) {
    FakeBasecamp bc;
    Tools t = makeTools(bc);
    CallResult r = t.run({{"tool", "list_available"}, {"args", {{"query", "wallet"}}}}, noProgress);
    LOGOS_ASSERT_TRUE(r.ok);
    LOGOS_ASSERT_EQ(r.value["packages"].size(), size_t(2));
}

LOGOS_TEST(status_lists_running_modules) {
    FakeBasecamp bc;
    runningEthRpc(bc);
    Tools t = makeTools(bc);
    CallResult r = t.run({{"tool", "status"}, {"args", Json::object()}}, noProgress);
    LOGOS_ASSERT_TRUE(r.ok);
    const Json& running = r.value["running"];
    LOGOS_ASSERT_TRUE(std::find(running.begin(), running.end(), Json("eth_rpc_module")) != running.end());
    LOGOS_ASSERT_TRUE(std::find(running.begin(), running.end(), Json("eth_rpc_ui")) == running.end());
}

LOGOS_TEST(add_a_repository_then_list_what_it_has) {
    FakeBasecamp bc;
    std::string added;
    bc.handlers["package_downloader.addRepository"] = [&](const Json& a) { added = a[0]; return Json{{"success", true}}; };
    bc.handlers["package_downloader.getCatalogForRepo"] = [](const Json&) {
        return Json::array({{{"name", "kym"}, {"type", "ui_qml"}}, {{"name", "kym_core"}, {"type", "core"}}});
    };
    Tools t = makeTools(bc);
    const std::string url = "https://jimmy-crib.office.mesh:8444/basecamp-0.3/logos-repo.json";
    Prepared p = t.prepare({{"tool", "add_repository"}, {"args", {{"url", url}}}});
    LOGOS_ASSERT_TRUE(p.ok);
    LOGOS_ASSERT_TRUE(p.mutating);
    LOGOS_ASSERT_CONTAINS(p.description, std::string("unsigned"));
    LOGOS_ASSERT_FALSE(t.prepare({{"tool", "add_repository"}, {"args", {{"url", "file:///etc/passwd"}}}}).ok);
    LOGOS_ASSERT_TRUE(t.run(p.step, noProgress).ok);
    LOGOS_ASSERT_EQ(added, url);
    CallResult l = t.run({{"tool", "list_available"}, {"args", {{"query", ""}, {"repository", url}}}}, noProgress);
    LOGOS_ASSERT_TRUE(l.ok);
    LOGOS_ASSERT_EQ(l.value["summary"].get<std::string>(), url + " has 2 packages.");
    LOGOS_ASSERT_EQ(l.value["items"].size(), size_t(2));
    LOGOS_ASSERT_EQ(l.value["items"][0]["name"].get<std::string>(), std::string("kym"));
    LOGOS_ASSERT_EQ(l.value["items"][0]["note"].get<std::string>(), std::string("app"));
    LOGOS_ASSERT_EQ(l.value["forModel"].get<std::string>(), url + " has: 1. kym (app), 2. kym_core (module).");
}

LOGOS_TEST(a_repository_that_refuses_is_reported) {
    FakeBasecamp bc;
    bc.handlers["package_downloader.addRepository"] = [](const Json&) { return Json{{"success", false}, {"error", "not a valid repo URL"}}; };
    Tools t = makeTools(bc);
    CallResult r = t.run({{"tool", "add_repository"}, {"args", {{"url", "https://x.example/logos-repo.json"}}}}, noProgress);
    LOGOS_ASSERT_FALSE(r.ok);
    LOGOS_ASSERT_CONTAINS(r.error, std::string("not a valid repo URL"));
}
