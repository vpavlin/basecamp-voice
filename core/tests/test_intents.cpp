// App intents (docs/adr/0009-app-intents.md): what installed apps provide,
// how a step is checked, raised through the view and reported.

#include <logos_test.h>

#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>

#include "engine.h"
#include "fake_basecamp.h"
#include "model_planner.h"

namespace {

// Two installed apps on disk, as Basecamp lays them out: our view declares
// what it may raise (uses), Scala's view what it offers (provides).
struct IntentFixture {
    FakeBasecamp bc;
    std::string root;

    explicit IntentFixture(const std::string& name) {
        root = "/tmp/basecamp_voice_intents_" + name + "_" + std::to_string(::getpid());
        std::system(("rm -rf '" + root + "' && mkdir -p '" + root + "/basecamp_voice' '" + root + "/scala_ui'").c_str());
        write("basecamp_voice", R"({"name":"basecamp_voice","type":"ui_qml","uses":[
            {"intent":"basecamp.apps.launch"},{"intent":"scala.event.create"},{"intent":"scala.events.list"},
            {"intent":"scala.calendar.share"}, "scala.calendar.create"]})");
        write("scala_ui", R"({"name":"scala_ui","type":"ui_qml","provides":[
            {"intent":"scala.events.list","readOnly":true,"description":"List the events in a time range.",
             "params":[{"name":"from","type":"string","required":false,"description":"Start: 2026-10-05."},
                       {"name":"calendar","type":"string","required":false}]},
            {"intent":"scala.event.create","description":"Add an event to a calendar.",
             "params":[{"name":"title","type":"string","required":true,"description":"What the event is."},
                       {"name":"start","type":"string","required":true},
                       {"name":"calendar","type":"string","required":false},
                       {"name":"reminder","type":"number","required":false}]},
            {"intent":"scala.calendar.share","handoff":true,"description":"Open the share dialog."},
            {"intent":"scala.calendar.create","description":"Not in our uses as an object."},
            {"intent":"scala.calendar.join","description":"Not in our uses at all."}]})");
        bc.catalog["basecamp_voice"] = {"ui_qml", 1, {"basecamp_voice_core"}};
        bc.catalog["scala_ui"] = {"ui_qml", 1, {"scala"}};
        bc.catalog["scala"] = {"core", 1, {}};
        for (const char* n : {"basecamp_voice", "scala_ui", "scala"}) bc.installed.insert(n);
        bc.installDirs["basecamp_voice"] = root + "/basecamp_voice";
        bc.installDirs["scala_ui"] = root + "/scala_ui";
    }
    ~IntentFixture() { std::system(("rm -rf '" + root + "'").c_str()); }

    void write(const std::string& app, const std::string& json) {
        std::ofstream(root + "/" + app + "/metadata.json") << json;
    }
};

Json step(const std::string& intent, Json params) {
    return {{"tool", "intent"}, {"args", {{"intent", intent}, {"params", std::move(params)}}}};
}

}  // namespace

LOGOS_TEST(intents_are_what_installed_apps_provide_and_we_declare) {
    IntentFixture f("discover");
    Tools t(f.bc, [](const std::string&) { return CallResult{}; });
    const auto all = t.intents();
    std::set<std::string> names;
    for (const auto& a : all) names.insert(a.intent);
    // join is not in our uses; create is only a bare string there, which
    // Basecamp ignores too.
    LOGOS_ASSERT_TRUE(names == (std::set<std::string>{"scala.events.list", "scala.event.create", "scala.calendar.share"}));
    for (const auto& a : all) {
        LOGOS_ASSERT_EQ(a.app, std::string("scala_ui"));
        if (a.intent == "scala.events.list") LOGOS_ASSERT_TRUE(a.readOnly && !a.handoff);
        if (a.intent == "scala.calendar.share") LOGOS_ASSERT_TRUE(a.handoff && !a.readOnly);
        if (a.intent == "scala.event.create") {
            LOGOS_ASSERT_EQ(a.params.size(), size_t(4));
            LOGOS_ASSERT_TRUE(a.params[0].required && a.params[0].name == "title");
            LOGOS_ASSERT_EQ(a.params[3].type, std::string("number"));
        }
    }
    // Uninstalled: nothing is offered.
    f.bc.installed.erase("scala_ui");
    LOGOS_ASSERT_TRUE(t.intents().empty());
}

LOGOS_TEST(intent_step_is_checked_against_what_the_app_describes) {
    IntentFixture f("prepare");
    Tools t(f.bc, [](const std::string&) { return CallResult{}; });

    Prepared p = t.prepare(step("scala.event.create", {{"title", "Dentist"}, {"start", "2026-10-06T15:00"},
                                                       {"calendar", ""}, {"invented", "x"}}));
    LOGOS_ASSERT_TRUE(p.ok);
    LOGOS_ASSERT_TRUE(p.mutating);
    // The empty optional and the parameter the app does not describe are left out.
    LOGOS_ASSERT_TRUE(p.step["args"]["params"] == Json({{"title", "Dentist"}, {"start", "2026-10-06T15:00"}}));
    LOGOS_ASSERT_EQ(p.description, std::string("Ask scala_ui: Add an event to a calendar (title: Dentist, start: 2026-10-06T15:00)"));

    Prepared missing = t.prepare(step("scala.event.create", {{"title", "Dentist"}}));
    LOGOS_ASSERT_FALSE(missing.ok);
    LOGOS_ASSERT_CONTAINS(missing.error, std::string("needs start"));

    Prepared typed = t.prepare(step("scala.event.create", {{"title", "Dentist"}, {"start", "x"}, {"reminder", "ten"}}));
    LOGOS_ASSERT_FALSE(typed.ok);
    LOGOS_ASSERT_CONTAINS(typed.error, std::string("reminder should be a number"));

    Prepared unknown = t.prepare(step("scala.calendar.join", {{"link", "scala://join?id=x"}}));
    LOGOS_ASSERT_FALSE(unknown.ok);
    LOGOS_ASSERT_CONTAINS(unknown.error, std::string("No installed app offers"));

    // The app's word that it only reads: no confirmation of ours.
    Prepared read = t.prepare(step("scala.events.list", Json::object()));
    LOGOS_ASSERT_TRUE(read.ok);
    LOGOS_ASSERT_FALSE(read.mutating);
}

LOGOS_TEST(intent_answer_with_a_list_becomes_the_numbered_items) {
    IntentFixture f("run");
    Tools t(f.bc, [](const std::string&) { return CallResult{}; });
    Json raised;
    t.raiseIntent = [&](const std::string& intent, const Json& params) {
        raised = {{"intent", intent}, {"params", params}};
        CallResult r;
        r.ok = true;
        r.value = {{"ok", true}, {"error", ""}, {"data", {{"events", Json::array({
            {{"title", "Dentist"}, {"start", "2026-10-06 15:00"}, {"end", "2026-10-06 16:00"}, {"calendar", "Personal"}},
            {{"title", "Trip"}, {"start", "2026-10-09"}, {"allDay", true}, {"calendar", "Family"}}})}, {"more", false}}}};
        return r;
    };
    CallResult r = t.run(step("scala.events.list", {{"from", "2026-10-06"}}), [](const std::string&) {});
    LOGOS_ASSERT_TRUE(r.ok);
    LOGOS_ASSERT_TRUE(raised == Json({{"intent", "scala.events.list"}, {"params", {{"from", "2026-10-06"}}}}));
    LOGOS_ASSERT_EQ(r.value["items"].size(), size_t(2));
    LOGOS_ASSERT_EQ(r.value["items"][0]["name"].get<std::string>(), std::string("Dentist"));
    LOGOS_ASSERT_EQ(r.value["items"][0]["note"].get<std::string>(), std::string("2026-10-06 15:00–16:00, Personal"));
    LOGOS_ASSERT_EQ(r.value["summary"].get<std::string>(), std::string("scala_ui listed 2 events."));
    LOGOS_ASSERT_CONTAINS(r.value["forModel"].get<std::string>(), std::string("1. Dentist ("));
    LOGOS_ASSERT_CONTAINS(r.value["forModel"].get<std::string>(), std::string("2. Trip (2026-10-09, allDay, Family)"));
}

LOGOS_TEST(intent_refusals_say_what_happened) {
    IntentFixture f("codes");
    Tools t(f.bc, [](const std::string&) { return CallResult{}; });
    std::string code;
    t.raiseIntent = [&](const std::string&, const Json&) {
        CallResult r;
        r.ok = true;
        r.value = {{"ok", false}, {"data", Json::object()}, {"error", code}};
        return r;
    };
    auto run = [&](const std::string& c) {
        code = c;
        return t.run(step("scala.event.create", {{"title", "X"}, {"start", "2026-10-06"}}), [](const std::string&) {});
    };
    LOGOS_ASSERT_EQ(run("cancelled").error, std::string("Cancelled in Basecamp's confirmation."));
    LOGOS_ASSERT_CONTAINS(run("bad_request").error, std::string("its window says why"));
    LOGOS_ASSERT_CONTAINS(run("unavailable").error, std::string("Basecamp found no app"));
    LOGOS_ASSERT_FALSE(run("failed").ok);

    // An object in the answer is the summary.
    t.raiseIntent = [](const std::string&, const Json&) {
        CallResult r;
        r.ok = true;
        r.value = {{"ok", true}, {"error", ""}, {"data", {{"event", {{"title", "Dentist"}, {"start", "2026-10-06 15:00"}, {"calendar", "Team"}}}}}};
        return r;
    };
    CallResult ok = t.run(step("scala.event.create", {{"title", "Dentist"}, {"start", "2026-10-06T15:00"}}), [](const std::string&) {});
    LOGOS_ASSERT_EQ(ok.value["summary"].get<std::string>(), std::string("scala_ui: event Dentist (2026-10-06 15:00, Team)."));
}

LOGOS_TEST(context_lists_the_intents_and_the_time) {
    IntentFixture f("context");
    Tools t(f.bc, [](const std::string&) { return CallResult{}; });
    t.nowText = [] { return std::string("Now: Sunday 2026-10-04 10:12 (local time)"); };
    const std::string c = t.context("add dentist on tuesday");
    LOGOS_ASSERT_CONTAINS(c, std::string("Context:\nNow: Sunday 2026-10-04 10:12 (local time)\n"));
    LOGOS_ASSERT_CONTAINS(c, std::string("- scala.event.create (scala_ui): Add an event to a calendar. Params: title: What the event is.; start; calendar?; reminder? (number)\n"));
    LOGOS_ASSERT_CONTAINS(c, std::string("- scala.events.list (scala_ui, reads only): "));
    LOGOS_ASSERT_TRUE(c.find("scala.calendar.join") == std::string::npos);
}

LOGOS_TEST(schema_narrows_the_intent_tool_to_each_intent_and_its_params) {
    IntentFixture f("schema");
    Tools t(f.bc, [](const std::string&) { return CallResult{}; });
    const Json s = ModelPlanner::schemaFor(t.intents());
    Json intentTool;
    for (const auto& v : s["properties"]["steps"]["items"]["anyOf"])
        if (v["properties"]["tool"]["const"] == "intent") intentTool = v;
    const Json& variants = intentTool["properties"]["args"]["anyOf"];
    LOGOS_ASSERT_EQ(variants.size(), size_t(3));
    Json create;
    for (const auto& v : variants) if (v["properties"]["intent"]["const"] == "scala.event.create") create = v;
    const Json& params = create["properties"]["params"];
    LOGOS_ASSERT_TRUE(params["required"] == Json::array({"title", "start"}));
    LOGOS_ASSERT_TRUE(params["properties"]["reminder"] == Json({{"type", "number"}}));
    LOGOS_ASSERT_FALSE(params["additionalProperties"].get<bool>());
}

namespace {

struct OnePlan : Planner {
    Json first;
    int nexts = 0;
    Json plan(const std::string&) override { return first; }
    Json planNext(const std::string&, const Json&) override {
        ++nexts;
        return {{"ok", true}, {"steps", Json::array()}, {"reply", "You have the dentist at 15:00."}};
    }
    bool continues() const override { return true; }
};

// Plays the view: takes each view action and answers it the way Basecamp would.
Json drive(Engine& e, const std::string& id, const std::vector<std::string>& states, Json* seen) {
    Json job;
    for (int i = 0; i < 400; ++i) {
        const Json ev = e.events(0);
        if (ev.contains("viewAction")) {
            *seen = ev["viewAction"];
            e.viewActionDone(ev["viewAction"]["id"],
                R"({"ok":true,"error":"","data":{"events":[{"title":"Dentist","start":"2026-10-06 15:00","calendar":"Team"}]}})");
        }
        for (const auto& j : ev["jobs"]) if (j["id"] == id) job = j;
        for (const auto& s : states) if (!job.is_null() && job["state"] == s) return job;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return job;
}

}  // namespace

LOGOS_TEST(read_intent_runs_without_asking_and_the_planner_sees_the_answer) {
    IntentFixture f("engine_read");
    OnePlan planner;
    planner.first = {{"ok", true}, {"reply", "Checking Scala."}, {"steps", Json::array({step("scala.events.list", {{"from", "2026-10-06"}})})}};
    Engine e(f.bc, planner);
    const std::string id = e.submit("what's on tuesday")["job"];
    Json seen;
    Json job = drive(e, id, {"done", "failed", "awaiting_confirmation"}, &seen);
    LOGOS_ASSERT_EQ(job["state"].get<std::string>(), std::string("done"));
    // Raised through the view, as the intent itself.
    LOGOS_ASSERT_EQ(seen["intent"].get<std::string>(), std::string("scala.events.list"));
    LOGOS_ASSERT_TRUE(seen["params"] == Json({{"from", "2026-10-06"}}));
    LOGOS_ASSERT_EQ(planner.nexts, 1);
    LOGOS_ASSERT_EQ(job["steps"][0]["items"][0]["name"].get<std::string>(), std::string("Dentist"));
    LOGOS_ASSERT_CONTAINS(job["summary"].get<std::string>(), std::string("You have the dentist at 15:00."));
}

LOGOS_TEST(write_intent_waits_for_do_it) {
    IntentFixture f("engine_write");
    OnePlan planner;
    planner.first = {{"ok", true}, {"reply", "Adding it."},
                     {"steps", Json::array({step("scala.event.create", {{"title", "Dentist"}, {"start", "2026-10-06T15:00"}})})}};
    Engine e(f.bc, planner);
    const std::string id = e.submit("add dentist on tuesday at 3")["job"];
    Json seen;
    Json job = drive(e, id, {"awaiting_confirmation", "done", "failed"}, &seen);
    LOGOS_ASSERT_EQ(job["state"].get<std::string>(), std::string("awaiting_confirmation"));
    LOGOS_ASSERT_TRUE(seen.is_null());
    e.confirm(id);
    job = drive(e, id, {"done", "failed"}, &seen);
    LOGOS_ASSERT_EQ(job["state"].get<std::string>(), std::string("done"));
    LOGOS_ASSERT_EQ(seen["intent"].get<std::string>(), std::string("scala.event.create"));
    // A write is not a reason for another round.
    LOGOS_ASSERT_EQ(planner.nexts, 0);
}

LOGOS_TEST(intent_without_the_window_open_fails_after_the_view_wait) {
    IntentFixture f("engine_nowindow");
    OnePlan planner;
    planner.first = {{"ok", true}, {"reply", "Checking."}, {"steps", Json::array({step("scala.events.list", Json::object())})}};
    Engine e(f.bc, planner);
    e.viewWaitMs = 100;
    const std::string id = e.submit("what's on today")["job"];
    Json job;
    for (int i = 0; i < 300 && (job.is_null() || (job["state"] != "failed" && job["state"] != "done")); ++i) {
        const Json ev = e.events(0, false);
        for (const auto& j : ev["jobs"]) if (j["id"] == id) job = j;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    LOGOS_ASSERT_EQ(job["state"].get<std::string>(), std::string("failed"));
    LOGOS_ASSERT_CONTAINS(job["summary"].get<std::string>(), std::string("window has to be open"));
}
