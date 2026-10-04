#include "voice_spike_impl.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>
#include <sys/stat.h>

#include <nlohmann/json.hpp>
#include "logos_sdk.h"

using nlohmann::json;

namespace {
const char* kDir = "/tmp/voice_spike";

long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

bool fileExists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}
}

VoiceSpikeImpl::VoiceSpikeImpl() = default;
VoiceSpikeImpl::~VoiceSpikeImpl() = default;

void VoiceSpikeImpl::log(const std::string& line) {
    ::mkdir(kDir, 0755);
    std::lock_guard<std::mutex> lk(m_mu);
    std::ofstream f(std::string(kDir) + "/log.jsonl", std::ios::app);
    f << line << "\n";
}

void VoiceSpikeImpl::onContextReady() {
    log(json{{"t", nowMs()}, {"event", "contextReady"}, {"module", moduleName()},
             {"instance", instanceId()}, {"modulePath", modulePath()},
             {"data", instancePersistencePath()}}.dump());
    // In the GUI there is no -c: a script dropped here runs on load.
    const std::string autorun = std::string(kDir) + "/autorun.json";
    if (fileExists(autorun)) runScript(autorun);
}

std::string VoiceSpikeImpl::whoami() {
    return json{{"module", moduleName()}, {"instance", instanceId()},
                {"modulePath", modulePath()}, {"data", instancePersistencePath()}}.dump();
}

std::string VoiceSpikeImpl::callImpl(const std::string& module, const std::string& method, const std::string& argsJson) {
    json args = json::parse(argsJson.empty() ? "[]" : argsJson, nullptr, false);
    if (args.is_discarded() || !args.is_array())
        return json{{"ok", false}, {"error", "argsJson must be a JSON array"}}.dump();
    const long long t0 = nowMs();
    logos::CallError err;
    json result = modules().dynamic(module).invoke(method, args, &err, 120000);
    json out{{"module", module}, {"method", method}, {"args", args}, {"ms", nowMs() - t0}};
    if (!err.code.empty()) {
        out["ok"] = false;
        out["error"] = json{{"code", err.code}, {"message", err.message}, {"origin", err.origin}};
    } else {
        out["ok"] = true;
        out["result"] = result;
    }
    return out.dump();
}

std::string VoiceSpikeImpl::call(const std::string& module, const std::string& method, const std::string& argsJson) {
    std::string r = callImpl(module, method, argsJson);
    log(r);
    return r;
}

std::string VoiceSpikeImpl::methods(const std::string& module) {
    json m = modules().dynamic(module).getMethods();
    log(json{{"t", nowMs()}, {"methodsOf", module}, {"result", m}}.dump());
    return m.dump();
}

std::string VoiceSpikeImpl::installByName(const std::string& name) {
    json out{{"install", name}};
    json installed = json::parse(callImpl("package_manager", "getInstalledPackages", "[]"));
    json have = json::array();
    if (installed.value("ok", false) && installed["result"].is_array())
        for (const auto& p : installed["result"])
            have.push_back({{"name", p.value("name", "")}, {"version", p.value("version", "")},
                            {"rootHash", p.contains("hashes") ? p["hashes"].value("root", "") : ""}});
    json deps = json::array({json{{"name", name}}});
    json dl = json::parse(callImpl("package_downloader", "downloadResolvedDependencies",
                                   json::array({deps.dump(), have.dump()}).dump()));
    out["download"] = dl;
    json results = json::array();
    if (dl.value("ok", false) && dl["result"].is_array()) {
        for (const auto& row : dl["result"]) {
            if (row.contains("error") || !row.contains("path")) { results.push_back(row); continue; }
            results.push_back(json::parse(callImpl("package_manager", "installPlugin",
                json::array({row["path"], false, row.value("source", "")}).dump())));
        }
    }
    out["installs"] = results;
    log(out.dump());
    return out.dump();
}

std::string VoiceSpikeImpl::nextAction(const std::string&) {
    std::lock_guard<std::mutex> lk(m_actMu);
    std::string a = m_pendingAction.empty() ? "{}" : m_pendingAction;
    m_pendingAction.clear();
    return a;
}

std::string VoiceSpikeImpl::note(const std::string& text) {
    log(json{{"t", nowMs()}, {"viewNote", text}}.dump());
    return "{}";
}

std::string VoiceSpikeImpl::runScript(const std::string& path) {
    if (!fileExists(path)) return json{{"ok", false}, {"error", "no such file"}}.dump();
    std::thread([this, path]() { runSteps(path); }).detach();
    return json{{"ok", true}, {"started", path}}.dump();
}

// Script: {"steps":[ ... "untilContains":["a","b"] retries until the result has one,{"waitScript":path} (block until it exists, run it),{"module":..,"method":..,"args":[..],"retry":N,"delayMs":ms,
//                    "until":"ok"|"truthy"}, {"sleepMs":ms}, {"methods":"<module>"}]}
void VoiceSpikeImpl::runSteps(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    json script = json::parse(ss.str(), nullptr, false);
    if (script.is_discarded()) { log(R"({"script":"parse error"})"); return; }
    log(json{{"t", nowMs()}, {"script", "start"}, {"path", path}}.dump());
    int i = 0;
    for (const auto& step : script.value("steps", json::array())) {
        ++i;
        if (step.contains("sleepMs")) {
            std::this_thread::sleep_for(std::chrono::milliseconds(step["sleepMs"].get<int>()));
            continue;
        }
        if (step.contains("methods")) {
            json m = modules().dynamic(step["methods"].get<std::string>()).getMethods();
            log(json{{"t", nowMs()}, {"step", i}, {"methodsOf", step["methods"]}, {"result", m}}.dump());
            continue;
        }
        if (step.contains("waitScript")) {
            const std::string next = step["waitScript"].get<std::string>();
            while (!fileExists(next)) std::this_thread::sleep_for(std::chrono::seconds(1));
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            log(json{{"t", nowMs()}, {"step", i}, {"continuingWith", next}}.dump());
            runSteps(next);
            continue;
        }
        if (step.contains("install")) {
            json r = json::parse(installByName(step["install"].get<std::string>()));
            log(json{{"t", nowMs()}, {"step", i}, {"installed", step["install"]}}.dump());
            continue;
        }
        if (step.contains("viewAction")) {
            std::lock_guard<std::mutex> lk(m_actMu);
            m_pendingAction = step["viewAction"].dump();
            log(json{{"t", nowMs()}, {"step", i}, {"queuedViewAction", step["viewAction"]}}.dump());
            continue;
        }
        const int retry = step.value("retry", 0);
        const int delayMs = step.value("delayMs", 1000);
        const std::string until = step.value("until", "ok");
        for (int attempt = 0; attempt <= retry; ++attempt) {
            json r = json::parse(callImpl(step["module"], step["method"], step.value("args", json::array()).dump()));
            r["t"] = nowMs();
            r["step"] = i;
            r["attempt"] = attempt;
            log(r.dump());
            bool done = r["ok"].get<bool>();
            if (done && step.contains("untilContains")) {
                // Any of the given strings in the (unwrapped) result text.
                std::string text = r["result"].is_string() ? r["result"].get<std::string>() : r["result"].dump();
                done = false;
                for (const auto& needle : step["untilContains"])
                    if (text.find(needle.get<std::string>()) != std::string::npos) done = true;
            } else if (done && until == "truthy") {
                const json& v = r["result"];
                done = !(v.is_null() || v == false || (v.is_string() && v.get<std::string>().empty()));
            }
            if (done) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }
    }
    log(json{{"t", nowMs()}, {"script", "done"}}.dump());
}
