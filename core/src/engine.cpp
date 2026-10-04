#include "engine.h"
#include "text.h"

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <set>

namespace {

long long wallMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

Json fail(const std::string& error) { return {{"ok", false}, {"error", error}}; }

const size_t kMaxEvents = 2000;
const size_t kMaxJobs = 20;
const int kMaxRounds = 4;

}  // namespace

Engine::Engine(Bus& bus, Planner& planner) : m_bus(bus), m_planner(planner) {
    m_tools = std::make_unique<Tools>(m_bus, [this](const std::string& app) { return openAppThroughView(app); });
    m_tools->stopping = [this] { return m_stopFlag.load(); };
    m_tools->raiseIntent = [this](const std::string& intent, const Json& params) {
        std::string error;
        CallResult r = requestThroughView(intent, params, intentWaitMs, &error);
        if (!r.ok) r.error = error.empty() ? "the Basecamp Voice window has to be open to ask apps." : error;
        return r;
    };
    m_worker = std::thread([this]() { workerLoop(); });
}

Engine::~Engine() { stop(); }

void Engine::stop() {
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_stopping = true;
    }
    m_stopFlag = true;
    m_cv.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

void Engine::post(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_tasks.push_back(std::move(task));
    }
    m_cv.notify_all();
}

void Engine::workerLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(m_mu);
            m_cv.wait(lk, [this]() { return m_stopping || !m_tasks.empty(); });
            if (m_stopping) return;
            task = std::move(m_tasks.front());
            m_tasks.pop_front();
        }
        // Nothing may escape this thread: a throw here would end the module.
        try {
            task();
        } catch (const std::exception& e) {
            fprintf(stderr, "basecamp_voice_core: task failed: %s\n", e.what());
        } catch (...) {
            fprintf(stderr, "basecamp_voice_core: task failed\n");
        }
    }
}

// ---- called by the view (any thread) --------------------------------------

Json Engine::submit(const std::string& text) {
    std::string id;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_stopping) return fail("Basecamp Voice is shutting down.");
        id = "j" + std::to_string(++m_nextJob);
        Job job;
        job.id = id;
        job.text = text;
        job.state = "planning";
        m_jobs[id] = job;
        m_jobOrder.push_back(id);
        while (m_jobOrder.size() > kMaxJobs) {
            const std::string& old = m_jobOrder.front();
            const std::string st = m_jobs[old].state;
            if (st == "planning" || st == "running" || st == "awaiting_confirmation") break;
            m_jobs.erase(old);
            m_jobOrder.erase(m_jobOrder.begin());
        }
        record({{"type", "job"}, {"job", id}, {"text", text}, {"state", "planning"}});
    }
    post([this, id]() {
        try { planJob(id); } catch (const std::exception& e) { failJob(id, std::string("internal error: ") + e.what()); }
    });
    return {{"ok", true}, {"job", id}};
}

Json Engine::confirm(const std::string& jobId) {
    {
        std::lock_guard<std::mutex> lk(m_mu);
        auto it = m_jobs.find(jobId);
        if (it == m_jobs.end()) return fail("No such job.");
        if (it->second.state != "awaiting_confirmation") return fail("This plan is not waiting for confirmation.");
        setState(it->second, "running");
    }
    post([this, jobId]() {
        try { runJob(jobId); } catch (const std::exception& e) { failJob(jobId, std::string("internal error: ") + e.what()); }
    });
    return {{"ok", true}};
}

Json Engine::cancel(const std::string& jobId) {
    std::lock_guard<std::mutex> lk(m_mu);
    auto it = m_jobs.find(jobId);
    if (it == m_jobs.end()) return fail("No such job.");
    Job& job = it->second;
    if (job.state == "awaiting_confirmation" || job.state == "planning") {
        job.cancelRequested = true;
        for (auto& s : job.steps) if (s.status == "pending") s.status = "skipped";
        job.summary = "Cancelled. Nothing was changed.";
        setState(job, "cancelled");
        return {{"ok", true}};
    }
    if (job.state == "running") {
        // The step in progress finishes; the rest are skipped.
        job.cancelRequested = true;
        return {{"ok", true}, {"note", "Stopping after the current step."}};
    }
    return fail("This job has already finished.");
}

Json Engine::newConversation() {
    std::lock_guard<std::mutex> lk(m_mu);
    m_conversationStart = m_nextJob;
    record({{"type", "conversation"}, {"start", m_conversationStart}});
    return {{"ok", true}, {"conversationStart", m_conversationStart}};
}

Json Engine::history(size_t maxTurns) {
    std::lock_guard<std::mutex> lk(m_mu);
    Json out = Json::array();
    for (const auto& id : m_jobOrder) {
        const Job& j = m_jobs.at(id);
        if (std::atoi(id.c_str() + 1) <= m_conversationStart) continue;
        if (j.state != "done" && j.state != "failed" && j.state != "cancelled") continue;
        std::string lists;
        for (const auto& s : j.steps)
            if (s.status == "done" && !s.forModel.empty()) lists += (lists.empty() ? "" : " ") + s.forModel;
        out.push_back({{"said", j.text}, {"result", utf8Prefix(j.summary, 400)}, {"lists", utf8Prefix(lists, 1200)}});
    }
    while (out.size() > maxTurns) out.erase(out.begin());
    return out;
}

Json Engine::events(long long since, bool takeViewAction) {
    std::lock_guard<std::mutex> lk(m_mu);
    Json out = {{"ok", true}, {"seq", m_seq}, {"conversationStart", m_conversationStart}};
    Json evs = Json::array();
    for (const auto& e : m_events)
        if (e["seq"].get<long long>() > since) evs.push_back(e);
    out["events"] = evs;
    Json jobs = Json::array();
    for (const auto& id : m_jobOrder) jobs.push_back(jobJson(m_jobs.at(id)));
    out["jobs"] = jobs;
    for (auto& a : m_actions) {
        if (!takeViewAction) break;
        if (a.handedOut) continue;
        a.handedOut = true;
        Json action = a.action;
        action["id"] = a.id;
        out["viewAction"] = action;
        break;
    }
    return out;
}

Json Engine::viewActionDone(const std::string& actionId, const std::string& resultJson) {
    {
        std::lock_guard<std::mutex> lk(m_mu);
        bool found = false;
        for (auto& a : m_actions) {
            if (a.id != actionId) continue;
            Json r = Tools::unwrap(Json(resultJson));
            a.result = r.is_discarded() ? Json(resultJson) : r;
            a.answered = true;
            found = true;
        }
        if (!found) return fail("No such view action (it may have timed out).");
    }
    m_cv.notify_all();
    return {{"ok", true}};
}

// ---- worker thread ----------------------------------------------------------

void Engine::failJob(const std::string& jobId, const std::string& why) {
    std::lock_guard<std::mutex> lk(m_mu);
    auto it = m_jobs.find(jobId);
    if (it == m_jobs.end()) return;
    Job& job = it->second;
    if (job.state == "done" || job.state == "failed" || job.state == "cancelled") return;
    for (auto& s : job.steps) if (s.status == "pending" || s.status == "running") s.status = s.status == "running" ? "failed" : "skipped";
    job.summary = "Failed: " + why;
    setState(job, "failed");
}

void Engine::planJob(const std::string& jobId) {
    std::string text;
    Json done = Json::array();
    int round = 0;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        auto it = m_jobs.find(jobId);
        if (it == m_jobs.end() || it->second.cancelRequested) return;
        text = it->second.text;
        round = it->second.round;
        for (const auto& s : it->second.steps)
            if (s.status == "done") done.push_back({{"step", s.description}, {"result", s.forModel.empty() ? s.detail : s.forModel}});
    }
    const auto steady = [] {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    };
    const long long t0 = steady();
    const Json plan = round == 0 ? m_planner.plan(text) : m_planner.planNext(text, done);
    const long long t1 = steady();
    std::vector<Step> steps;
    std::string error;
    const std::string reply = plan.contains("reply") && plan["reply"].is_string() ? plan["reply"].get<std::string>() : std::string();
    const bool planned = plan.contains("ok") && plan["ok"].is_boolean() && plan["ok"].get<bool>();
    if (!planned) {
        error = plan.contains("error") && plan["error"].is_string() ? plan["error"].get<std::string>() : std::string("I could not plan that.");
    } else {
        // open_app installs what it opens: an install of the same app in the
        // same plan is redundant (small models add one anyway). Exact repeats
        // of a step are dropped too.
        std::set<std::string> opened, seen;
        std::vector<Prepared> prepared;
        const Json noSteps = Json::array();
        const Json& planSteps = plan.contains("steps") && plan["steps"].is_array() ? plan["steps"] : noSteps;
        for (const auto& s : planSteps) {
            Prepared p = m_tools->prepare(s);
            if (!p.ok) { error = p.error; break; }
            if (!seen.insert(safeDump(p.step)).second) continue;
            if (p.step.value("tool", "") == "open_app") opened.insert(p.step["args"].value("app", ""));
            prepared.push_back(p);
        }
        // "Check whether X is installed, then start it": the context already
        // says what is installed and running, so a list/status step in a plan
        // that also acts only delays it (and costs a round). Drop it.
        bool acts = false;
        for (const auto& p : prepared) {
            const std::string t = p.step.value("tool", "");
            acts = acts || t == "install" || t == "open_app" || t == "recipe" || t == "call" || t == "add_repository" ||
                   (t == "intent" && p.mutating);
        }
        for (const auto& p : prepared) {
            if (!error.empty()) break;
            const std::string tool = p.step.value("tool", "");
            // ...but listing a repository just added is the point, not a check.
            const bool repoListing = tool == "list_available" && !p.step["args"].value("repository", "").empty();
            if (acts && !repoListing && (tool == "list_installed" || tool == "list_available" || tool == "status")) continue;
            if (p.step.value("tool", "") == "install" && opened.count(p.step["args"].value("name", ""))) continue;
            Step st;
            st.step = p.step;
            st.description = p.description;
            st.mutating = p.mutating;
            steps.push_back(st);
        }
    }

    const long long t2 = steady();
    bool needsConfirmation = false;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        auto it = m_jobs.find(jobId);
        if (it == m_jobs.end() || it->second.cancelRequested) return;
        Job& job = it->second;
        job.planMs += t1 - t0;
        job.prepareMs += t2 - t1;
        // Sum what the model server reported (tokens read/written and the
        // milliseconds each took) over the planning rounds.
        if (plan.contains("stats") && plan["stats"].is_object()) {
            for (const auto& [k, v] : plan["stats"].items()) {
                if (!v.is_number() || v.get<double>() < 0) continue;
                const double before = job.model.contains(k) ? job.model[k].get<double>() : 0.0;
                job.model[k] = before + v.get<double>();
            }
            job.model["requests"] = (job.model.contains("requests") ? job.model["requests"].get<int>() : 0) + 1;
        }
        if (!error.empty()) {
            if (round == 0) {
                job.summary = error;
                setState(job, "failed");
                return;
            }
            // The work itself is done; only the follow-up planning failed.
            // Report what was done, and why the rest could not be planned.
            job.reply = "(Could not plan anything further: " + utf8Prefix(error, 200) + ")";
            finish(job, job.runMs, true);
            return;
        }
        job.reply = reply;
        if (round > 0 && steps.empty()) {
            // Nothing left: the request is complete (or needs the user).
            finish(job, job.runMs, true);
            return;
        }
        job.roundStart = job.steps.size();
        for (const auto& s : steps) job.steps.push_back(s);
        for (const auto& s : steps) needsConfirmation = needsConfirmation || s.mutating;
        Json described = Json::array();
        for (const auto& s : steps) described.push_back({{"description", s.description}, {"mutating", s.mutating}});
        record({{"type", "plan"}, {"job", jobId}, {"round", round}, {"steps", described},
                {"needsConfirmation", needsConfirmation}, {"reply", reply}});
        setState(job, needsConfirmation ? "awaiting_confirmation" : "running");
    }
    if (!needsConfirmation) runJob(jobId);
}

// The summary of everything that ran, then the planner's closing sentence
// when the last round added nothing.
void Engine::finish(Job& job, long long runMs, bool closingReply) {
    std::string all;
    for (const auto& s : job.steps)
        if (s.status == "done" && !s.detail.empty()) all += (all.empty() ? "" : " ") + s.detail;
    if (closingReply && !job.reply.empty()) all += (all.empty() ? "" : " ") + job.reply;
    job.summary = !all.empty() ? all : (!job.reply.empty() ? job.reply : "Done.");
    job.runMs = runMs;
    setState(job, "done");
}

void Engine::runJob(const std::string& jobId) {
    using namespace std::chrono;
    const auto started = steady_clock::now();
    long long before = 0;
    auto elapsed = [&] { return before + duration_cast<milliseconds>(steady_clock::now() - started).count(); };
    size_t i = 0;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        auto it = m_jobs.find(jobId);
        if (it == m_jobs.end()) return;
        before = it->second.runMs;
        while (i < it->second.steps.size() && it->second.steps[i].status != "pending") ++i;
    }
    for (;; ++i) {
        Json step;
        {
            std::lock_guard<std::mutex> lk(m_mu);
            auto it = m_jobs.find(jobId);
            if (it == m_jobs.end()) return;
            Job& job = it->second;
            if (i >= job.steps.size()) {
                // Opening an app, or reading anything (methods, the catalog,
                // what is installed or running), is a means to an end: ask
                // the planner what is left, now that it has seen the result
                // (at most kMaxRounds rounds).
                bool informative = false;
                for (size_t k = job.roundStart; k < job.steps.size(); ++k) {
                    const std::string tool = job.steps[k].step.value("tool", "");
                    informative = informative || tool == "open_app" || tool == "module_methods" ||
                                  tool == "list_installed" || tool == "list_available" || tool == "status" ||
                                  (tool == "intent" && !job.steps[k].mutating);
                }
                if (informative && m_planner.continues() && job.round + 1 < kMaxRounds && !job.cancelRequested) {
                    job.round += 1;
                    job.runMs = elapsed();
                    setState(job, "planning");
                    break;   // plan the next round below, without m_mu
                }
                finish(job, elapsed(), false);
                return;
            }
            if (job.cancelRequested) {
                for (size_t k = i; k < job.steps.size(); ++k) job.steps[k].status = "skipped";
                std::string all;
                for (const auto& s : job.steps) if (s.status == "done" && !s.detail.empty()) all += s.detail + " ";
                job.summary = all + "Stopped before: " + job.steps[i].description + ".";
                job.runMs = elapsed();
                setState(job, "cancelled");
                return;
            }
            job.steps[i].status = "running";
            step = job.steps[i].step;
            record({{"type", "step"}, {"job", jobId}, {"index", i}, {"status", "running"},
                  {"description", job.steps[i].description}});
        }

        auto progress = [this, jobId, i](const std::string& message) {
            std::lock_guard<std::mutex> lk(m_mu);
            auto it = m_jobs.find(jobId);
            if (it != m_jobs.end() && i < it->second.steps.size()) it->second.steps[i].detail = message;
            record({{"type", "progress"}, {"job", jobId}, {"index", i}, {"message", message}});
        };
        CallResult r = m_tools->run(step, progress);

        std::lock_guard<std::mutex> lk(m_mu);
        auto it = m_jobs.find(jobId);
        if (it == m_jobs.end()) return;
        Job& job = it->second;
        Step& s = job.steps[i];
        if (!r.ok) {
            s.status = "failed";
            s.detail = r.error;
            for (size_t k = i + 1; k < job.steps.size(); ++k) job.steps[k].status = "skipped";
            record({{"type", "step"}, {"job", jobId}, {"index", i}, {"status", "failed"}, {"error", r.error}});
            std::string all;
            for (const auto& x : job.steps) if (x.status == "done" && !x.detail.empty()) all += x.detail + " ";
            job.summary = all + "Failed: " + r.error;
            job.runMs = elapsed();
            setState(job, "failed");
            return;
        }
        const std::string summary = r.value.value("summary", std::string());
        s.status = "done";
        s.detail = summary;
        if (r.value.contains("items") && r.value["items"].is_array()) s.items = r.value["items"];
        s.forModel = r.value.value("forModel", std::string());
        Json ev = {{"type", "step"}, {"job", jobId}, {"index", i}, {"status", "done"}, {"summary", summary}};
        Json result = r.value;
        result.erase("summary");
        result.erase("forModel");
        if (safeDump(result).size() <= 20000) ev["result"] = result;
        record(ev);
    }
    planJob(jobId);
}

CallResult Engine::openAppThroughView(const std::string& app) {
    std::string error;
    CallResult r = requestThroughView("basecamp.apps.launch", {{"app", app}}, viewWaitMs, &error);
    if (!r.ok) {
        r.error = error == "no answer" ? "Basecamp did not answer the request to open " + app + "."
                                       : "the Basecamp Voice window has to be open to open apps.";
        return r;
    }
    const Json& res = r.value;
    if (res.is_object() && res.value("ok", false)) return r;
    std::string err = res.is_object() ? res.value("error", std::string()) : std::string();
    CallResult f;
    f.error = err.empty() ? "Basecamp refused to open it." : "Basecamp said " + err + ".";
    return f;
}

CallResult Engine::requestThroughView(const std::string& intent, const Json& params, int waitMs, std::string* error) {
    std::unique_lock<std::mutex> lk(m_mu);
    ViewAction a;
    a.id = "a" + std::to_string(++m_nextAction);
    a.action = {{"intent", intent}, {"params", params}};
    const std::string id = a.id;
    m_actions.push_back(a);

    auto mine = [this, &id]() -> ViewAction* {
        for (auto& x : m_actions) if (x.id == id) return &x;
        return nullptr;
    };
    // The view takes the action on its next poll; if nobody takes it within
    // viewWaitMs, the window is not open. Once taken, wait the full waitMs.
    const auto start = std::chrono::steady_clock::now();
    m_cv.wait_for(lk, std::chrono::milliseconds(std::min(waitMs, viewWaitMs)),
                  [&]() { return m_stopping || mine()->answered; });
    if (!m_stopping && !mine()->answered && mine()->handedOut) {
        const auto left = std::chrono::milliseconds(waitMs) -
                          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        if (left.count() > 0)
            m_cv.wait_for(lk, left, [&]() { return m_stopping || mine()->answered || m_stopFlag.load(); });
    }
    ViewAction done = *mine();
    for (auto it = m_actions.begin(); it != m_actions.end(); ++it)
        if (it->id == id) { m_actions.erase(it); break; }

    CallResult r;
    if (!done.answered) {
        *error = done.handedOut ? "no answer" : "";
        if (done.handedOut && intent != "basecamp.apps.launch") *error = "No answer from Basecamp about " + intent + ".";
        return r;
    }
    r.ok = true;
    r.value = done.result;
    return r;
}

// ---- helpers (m_mu held) ----------------------------------------------------

void Engine::record(Json event) {
    event["seq"] = ++m_seq;
    event["t"] = wallMs();
    m_events.push_back(std::move(event));
    if (m_events.size() > kMaxEvents) m_events.erase(m_events.begin(), m_events.begin() + (m_events.size() - kMaxEvents));
}

void Engine::setState(Job& job, const std::string& state) {
    job.state = state;
    Json ev = {{"type", "state"}, {"job", job.id}, {"state", state}};
    if (!job.summary.empty()) ev["summary"] = job.summary;
    record(ev);
}

Json Engine::jobJson(const Job& job) const {
    Json steps = Json::array();
    for (const auto& s : job.steps)
        steps.push_back({{"description", s.description}, {"mutating", s.mutating}, {"items", s.items},
                         {"status", s.status}, {"detail", s.detail}});
    return {{"id", job.id}, {"text", job.text}, {"state", job.state}, {"steps", steps},
            {"summary", job.summary}, {"reply", job.reply},
            {"timing", {{"planMs", job.planMs}, {"prepareMs", job.prepareMs}, {"runMs", job.runMs}, {"model", job.model}}}};
}
