#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "bus.h"
#include "planner.h"
#include "tools.h"

// Runs commands as jobs: plan -> (confirm) -> run each step -> summary.
//
// Everything that touches Basecamp happens on one worker thread, so the
// methods the view calls return at once; the view learns what happened by
// polling events(since). Job states:
//   planning -> awaiting_confirmation -> running -> done | failed
//            -> running (a plan that only reads runs without asking)
//            -> failed (no plan)          awaiting_confirmation -> cancelled
//
// Opening an app is the view's job (shell intents are a QML API): the worker
// queues a view action, the next events() reply hands it to the view, and the
// view reports back through viewActionDone().
class Engine {
public:
    Engine(Bus& bus, Planner& planner);
    ~Engine();

    Json submit(const std::string& text);
    Json confirm(const std::string& jobId);
    Json cancel(const std::string& jobId);
    // Events after `since` and the jobs. With takeViewAction (the view's poll),
    // also hands out at most one pending view action; nothing else may.
    Json events(long long since, bool takeViewAction = true);
    Json viewActionDone(const std::string& actionId, const std::string& resultJson);

    void stop();

    // A conversation: requests after the last newConversation() are shown to
    // the planner, so the user can refer back ("install the third one").
    Json newConversation();
    // Finished requests of this conversation, oldest first, at most maxTurns:
    // [{"said","result","lists"}].
    Json history(size_t maxTurns);

    // Runs a task on the engine's worker thread (it lives as long as the
    // module, which is what a process tied to its spawning thread needs).
    void post(std::function<void()> task);

    // How long open_app waits for the view to act and answer.
    int viewWaitMs = 20000;
    Tools& tools() { return *m_tools; }

private:
    struct Step {
        Json step;
        std::string description;
        bool mutating = false;
        std::string status = "pending";   // pending | running | done | failed | skipped
        std::string detail;
        Json items = Json::array();   // a listing's entries, for the view
        std::string forModel;         // what the next planning round is told
    };
    struct Job {
        std::string id;
        std::string text;
        std::string state;
        std::vector<Step> steps;
        std::string summary;
        std::string reply;          // the planner's sentence for the user
        long long planMs = 0;       // the planner (the model) took
        long long prepareMs = 0;    // resolving names and sizes took
        long long runMs = 0;        // running the steps took
        Json model = Json::object(); // the model's own figures, summed over rounds
        int round = 0;              // planning rounds after the first
        size_t roundStart = 0;      // index of this round's first step
        bool cancelRequested = false;
    };
    struct ViewAction {
        std::string id;
        Json action;
        bool handedOut = false;
        bool answered = false;
        Json result;
    };

    void workerLoop();
    void planJob(const std::string& jobId);
    void finish(Job& job, long long runMs, bool closingReply);   // m_mu held
    void failJob(const std::string& jobId, const std::string& why);
    void runJob(const std::string& jobId);
    CallResult openAppThroughView(const std::string& app);

    // Callers hold m_mu.
    void record(Json event);
    void setState(Job& job, const std::string& state);
    Json jobJson(const Job& job) const;

    Bus& m_bus;
    Planner& m_planner;
    std::unique_ptr<Tools> m_tools;

    std::mutex m_mu;
    std::condition_variable m_cv;
    std::map<std::string, Job> m_jobs;
    std::vector<std::string> m_jobOrder;
    std::vector<Json> m_events;
    long long m_seq = 0;
    int m_nextJob = 0;
    int m_conversationStart = 0;   // jobs numbered above this are in it
    int m_nextAction = 0;
    std::deque<ViewAction> m_actions;

    std::deque<std::function<void()>> m_tasks;
    bool m_stopping = false;
    std::atomic<bool> m_stopFlag{false};   // m_stopping, readable without m_mu
    std::thread m_worker;
};
