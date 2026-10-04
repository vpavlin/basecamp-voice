#pragma once

#include <mutex>
#include <string>
#include <logos_module_context.h>

// Spike for basecamp-voice: call any module by name, list its methods, and run
// a scripted sequence of calls on a background thread, logging every step to
// /tmp/voice_spike/log.jsonl. Declarations stay on one line each (codegen).
class VoiceSpikeImpl : public LogosModuleContext {
public:
    VoiceSpikeImpl();
    ~VoiceSpikeImpl();

    /// Call module.method with a JSON array of arguments; returns {ok,result,error,ms}.
    std::string call(const std::string& module, const std::string& method, const std::string& argsJson);

    /// The target module's getMethods() as JSON.
    std::string methods(const std::string& module);

    /// Run the JSON script at path on a background thread; returns immediately.
    std::string runScript(const std::string& path);

    /// Who am I (moduleName, instanceId, paths) as JSON.
    std::string whoami();

    /// Download name and its dependencies through package_downloader, install each through package_manager.
    std::string installByName(const std::string& name);

    /// For the spike view: the next action it should take in QML ({} when none).
    std::string nextAction(const std::string& unused);

    /// For the spike view: report what happened to the last action.
    std::string note(const std::string& text);

protected:
    void onContextReady() override;

private:
    std::string callImpl(const std::string& module, const std::string& method, const std::string& argsJson);
    void runSteps(const std::string& path);
    void log(const std::string& line);
    std::mutex m_mu;
    std::mutex m_actMu;
    std::string m_pendingAction;
};
