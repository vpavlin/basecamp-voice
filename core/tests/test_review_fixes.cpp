// Regression tests for the code review of 2026-10-03 (crashes and hangs).

#include <logos_test.h>

#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <thread>

#include "engine.h"
#include "fake_basecamp.h"
#include "model_planner.h"
#include "proc.h"
#include "text.h"
#include "voice.h"

namespace {
Json waitJob(Engine& e, const std::string& id) {
    Json job;
    for (int i = 0; i < 300; ++i) {
        const Json ev = e.events(0);
        for (const auto& j : ev["jobs"]) if (j["id"] == id) job = j;
        if (!job.is_null() && job["state"] != "planning" && job["state"] != "running") return job;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return job;
}
struct ThrowingPlanner : Planner {
    int calls = 0;
    Json plan(const std::string&) override {
        if (calls++ == 0) throw std::runtime_error("boom");
        return {{"ok", true}, {"steps", Json::array({{{"tool", "status"}, {"args", Json::object()}}})}, {"reply", ""}};
    }
};
}

LOGOS_TEST(utf8_cuts_never_split_a_character) {
    const std::string s = "ab\xc5\x99" "cd";   // a b r-caron c d
    LOGOS_ASSERT_EQ(utf8Prefix(s, 3), std::string("ab"));
    LOGOS_ASSERT_EQ(utf8Prefix(s, 4), std::string("ab\xc5\x99"));
    LOGOS_ASSERT_EQ(utf8Suffix(s, 3), std::string("cd"));
    LOGOS_ASSERT_EQ(utf8Suffix(s, 4), std::string("\xc5\x99" "cd"));
    LOGOS_ASSERT_EQ(safeDump(Json(std::string("x\xc5"))), std::string("\"x\xef\xbf\xbd\""));
}

LOGOS_TEST(a_non_ascii_description_cut_at_the_limit_does_not_crash_a_search) {
    FakeBasecamp bc;
    // r-caron straddles byte 140, where the search result cuts descriptions.
    std::string desc(139, 'a');
    desc += "\xc5\x99 wallet";
    bc.catalog["czech_wallet_ui"] = {"ui_qml", 1000, {}, desc};
    RulePlanner planner;
    Engine e(bc, planner);
    const std::string id = e.submit("search wallet")["job"];
    Json job = waitJob(e, id);
    // Before the fix, dumping the step result threw on the worker thread
    // and the module died.
    LOGOS_ASSERT_EQ(job["state"].get<std::string>(), std::string("done"));
    LOGOS_ASSERT_TRUE(safeDump(e.events(0)).find("aaa") != std::string::npos);
}

LOGOS_TEST(a_planner_that_throws_fails_its_job_and_the_engine_carries_on) {
    FakeBasecamp bc;
    ThrowingPlanner planner;
    Engine e(bc, planner);
    const std::string first = e.submit("anything")["job"];
    Json job = waitJob(e, first);
    LOGOS_ASSERT_EQ(job["state"].get<std::string>(), std::string("failed"));
    LOGOS_ASSERT_CONTAINS(job["summary"].get<std::string>(), std::string("boom"));
    const std::string second = e.submit("status")["job"];
    LOGOS_ASSERT_EQ(waitJob(e, second)["state"].get<std::string>(), std::string("done"));
}

LOGOS_TEST(a_model_reply_with_wrong_types_is_refused_not_thrown) {
    LOGOS_ASSERT_FALSE(ModelPlanner::parse(R"({"reply":42,"steps":"no"})")["ok"].get<bool>());
    Json p = ModelPlanner::parse(R"({"reply":null,"steps":[{"tool":"status","args":{}}]})");
    LOGOS_ASSERT_TRUE(p["ok"].get<bool>());
}

LOGOS_TEST(alive_looks_without_reaping) {
    std::string err;
    const pid_t pid = proc::spawn({"true"}, "", {}, &err);
    LOGOS_ASSERT_GT(pid, 0);
    for (int i = 0; i < 100 && proc::alive(pid); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    LOGOS_ASSERT_FALSE(proc::alive(pid));
    // Still ours to collect: the pid cannot have been reused.
    LOGOS_ASSERT_EQ(::waitpid(pid, nullptr, WNOHANG), pid);
}

LOGOS_TEST(wav_reader_refuses_devices_and_absurd_rates) {
    std::vector<float> pcm;
    std::string err;
    LOGOS_ASSERT_FALSE(readWav16k("/dev/zero", &pcm, &err));
    // 1 Hz would ask for a buffer 16000 times the samples.
    FILE* f = std::fopen("/tmp/basecamp_voice_rate1.wav", "wb");
    const unsigned char hdr[] = {'R','I','F','F',40,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,1,0,1,0,0,0,2,0,0,0,2,0,16,0,'d','a','t','a',4,0,0,0,1,0,2,0};
    std::fwrite(hdr, 1, sizeof hdr, f);
    std::fclose(f);
    LOGOS_ASSERT_FALSE(readWav16k("/tmp/basecamp_voice_rate1.wav", &pcm, &err));
    LOGOS_ASSERT_EQ(err, std::string("unsupported WAV format"));
}
