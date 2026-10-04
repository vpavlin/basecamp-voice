// ModelPlanner: what it sends to the model and how it reads the answer.

#include <logos_test.h>

#include <thread>

#include "engine.h"
#include "fake_basecamp.h"
#include "model_planner.h"

LOGOS_TEST(model_plan_is_parsed_into_steps_and_reply) {
    Json p = ModelPlanner::parse(R"({"reply":"Opening eth_rpc_ui.","steps":[{"tool":"open_app","args":{"app":"eth_rpc_ui"}}]})");
    LOGOS_ASSERT_TRUE(p["ok"].get<bool>());
    LOGOS_ASSERT_EQ(p["steps"][0]["tool"].get<std::string>(), std::string("open_app"));
    LOGOS_ASSERT_EQ(p["reply"].get<std::string>(), std::string("Opening eth_rpc_ui."));
}

LOGOS_TEST(model_answer_without_steps_is_a_reply) {
    Json p = ModelPlanner::parse(R"({"reply":"Which wallet?","steps":[]})");
    LOGOS_ASSERT_TRUE(p["ok"].get<bool>());
    LOGOS_ASSERT_EQ(p["steps"].size(), size_t(0));
    LOGOS_ASSERT_FALSE(ModelPlanner::parse("not json")["ok"].get<bool>());
    LOGOS_ASSERT_FALSE(ModelPlanner::parse(R"({"reply":"","steps":[]})")["ok"].get<bool>());
}

LOGOS_TEST(model_schema_allows_exactly_the_tools) {
    // Without app intents the intent tool is not offered at all.
    LOGOS_ASSERT_EQ(ModelPlanner::schema()["properties"]["steps"]["items"]["anyOf"].size(), size_t(9));
    AppIntent a;
    a.app = "scala_ui";
    a.intent = "scala.calendars.list";
    const Json withIntent = ModelPlanner::schemaFor({a});
    const Json& items = withIntent["properties"]["steps"]["items"]["anyOf"];
    LOGOS_ASSERT_EQ(items.size(), size_t(10));
    std::set<std::string> tools;
    for (const auto& i : items) tools.insert(i["properties"]["tool"]["const"].get<std::string>());
    for (const auto& t : Tools::names()) LOGOS_ASSERT_TRUE(tools.count(t) == 1);
}

LOGOS_TEST(model_planner_sends_the_prompt_and_the_context) {
    Json sent;
    ModelPlanner planner(
        [](const std::string& text) { return "Context:\nInstalled apps: none\n\nUser: \"" + text + "\""; },
        [&sent](const Json& messages, const Json& schema, std::string* content, std::string*, Json* stats) {
            *stats = {{"promptTokens", 900}, {"promptMs", 6000}, {"genTokens", 40}, {"genMs", 3600}};
            sent = {{"messages", messages}, {"schema", schema}};
            *content = R"({"reply":"Checking.","steps":[{"tool":"status","args":{}}]})";
            return true;
        });
    Json p = planner.plan("what's running");
    LOGOS_ASSERT_TRUE(p["ok"].get<bool>());
    LOGOS_ASSERT_EQ(sent["messages"][0]["content"].get<std::string>(), std::string(ModelPlanner::systemPrompt()));
    LOGOS_ASSERT_CONTAINS(sent["messages"][1]["content"].get<std::string>(), std::string("User: \"what's running\""));
    LOGOS_ASSERT_TRUE(sent["schema"] == ModelPlanner::schema());
    LOGOS_ASSERT_EQ(p["stats"]["genTokens"].get<int>(), 40);
}

LOGOS_TEST(model_planner_reports_an_unreachable_model) {
    ModelPlanner planner([](const std::string&) { return std::string(); },
                         [](const Json&, const Json&, std::string*, std::string* error, Json*) {
                             *error = "could not reach the model";
                             return false;
                         });
    Json p = planner.plan("x");
    LOGOS_ASSERT_FALSE(p["ok"].get<bool>());
    LOGOS_ASSERT_EQ(p["error"].get<std::string>(), std::string("could not reach the model"));
}

namespace {
struct FixedPlanner : Planner {
    Json answer;
    Json plan(const std::string&) override { return answer; }
};
}

LOGOS_TEST(engine_shows_a_reply_without_steps_as_the_answer) {
    FakeBasecamp bc;
    FixedPlanner planner;
    planner.answer = {{"ok", true}, {"steps", Json::array()}, {"reply", "There are three wallet apps. Which one?"}};
    Engine e(bc, planner);
    const std::string id = e.submit("open the wallet")["job"];
    Json job;
    for (int i = 0; i < 200; ++i) {
        const Json ev = e.events(0);
        job = ev["jobs"][0];
        if (job["state"] == "done" || job["state"] == "failed") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    LOGOS_ASSERT_EQ(job["state"].get<std::string>(), std::string("done"));
    LOGOS_ASSERT_EQ(job["summary"].get<std::string>(), std::string("There are three wallet apps. Which one?"));
    LOGOS_ASSERT_EQ(job["reply"].get<std::string>(), std::string("There are three wallet apps. Which one?"));
}

LOGOS_TEST(engine_drops_an_install_of_the_app_it_also_opens) {
    FakeBasecamp bc;
    FixedPlanner planner;
    planner.answer = {{"ok", true}, {"reply", "Installing and opening eth_rpc_ui."},
                      {"steps", Json::array({{{"tool", "install"}, {"args", {{"name", "eth_rpc_ui"}}}},
                                             {{"tool", "open_app"}, {"args", {{"app", "eth rpc"}}}}})}};
    Engine e(bc, planner);
    const std::string id = e.submit("install and open eth rpc")["job"];
    Json job;
    for (int i = 0; i < 200; ++i) {
        const Json ev = e.events(0);
        job = ev["jobs"][0];
        if (job["state"] != "planning") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    LOGOS_ASSERT_EQ(job["state"].get<std::string>(), std::string("awaiting_confirmation"));
    LOGOS_ASSERT_EQ(job["steps"].size(), size_t(1));
    LOGOS_ASSERT_CONTAINS(job["steps"][0]["description"].get<std::string>(), std::string("then open it"));
}

LOGOS_TEST(the_model_sees_the_conversation_before_the_request) {
    std::string sentUser;
    ModelPlanner planner(
        [](const std::string& text) { return "Context:\nInstalled apps: none\n\nUser: \"" + text + "\""; },
        [&](const Json& messages, const Json&, std::string* content, std::string*, Json*) {
            sentUser = messages[1]["content"];
            *content = R"({"reply":"Installing kym.","steps":[{"tool":"install","args":{"name":"kym"}}]})";
            return true;
        },
        [] { return Json::array({{{"said", "show me the apps in that repo"}, {"result", "It has 2 packages."},
                                  {"lists", "repo has: 1. kym (app), 2. kym_core (module)."}}}); });
    planner.plan("install the first one");
    LOGOS_ASSERT_CONTAINS(sentUser, std::string("Conversation so far"));
    LOGOS_ASSERT_CONTAINS(sentUser, std::string("Listed: repo has: 1. kym (app), 2. kym_core (module)."));
    LOGOS_ASSERT_TRUE(sentUser.find("Conversation so far") < sentUser.find("User: \"install the first one\""));
}
