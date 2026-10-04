#pragma once

#include <memory>
#include <string>

#include <logos_module_context.h>

class Engine;
class DynamicBus;
class Voice;

// basecamp_voice_core: turns what the user says into a plan, asks, and runs it.
// Everything runs inside Basecamp: the speech model in this process, the
// language model in a llama-server this module fetches and supervises.
// Every method returns at once with a JSON string ({"ok":true,...} or
// {"ok":false,"error":"..."}); work happens on background threads and the
// view polls events(). Keep each declaration on one line and at most 4 args.
class BasecampVoiceCoreImpl : public LogosModuleContext {
public:
    BasecampVoiceCoreImpl();
    ~BasecampVoiceCoreImpl();

    /// Plan a command (typed or transcribed). Returns {ok, job}.
    std::string submit(const std::string& text);

    /// Run a plan that is waiting for confirmation.
    std::string confirm(const std::string& jobId);

    /// Cancel a plan, or stop a running job after its current step.
    std::string cancel(const std::string& jobId);

    /// Events after sinceSeq, the jobs, setup, voice and model state, and a pending view action.
    std::string events(const std::string& sinceSeq);

    /// The view reports the reply to a view action (a shell intent).
    std::string viewActionDone(const std::string& actionId, const std::string& resultJson);

    /// Version, jobs, setup, voice and model state, for the first paint and diagnostics.
    std::string snapshot();

    /// Download what is missing (the user confirmed the sizes shown in setup).
    std::string startSetup();

    /// Stop the downloads; what was fetched so far is kept and resumed later.
    std::string cancelSetup();

    /// Start recording from the microphone.
    std::string recordStart();

    /// Stop recording, transcribe, and submit what was said.
    std::string recordStop();

    /// Stop recording and throw the recording away.
    std::string recordCancel();

    /// Transcribe a 16 kHz WAV file and submit what was said.
    std::string submitRecording(const std::string& wavPath);

    /// Start a new conversation: earlier requests no longer shape new ones.
    std::string newConversation();

    /// Read settings (empty argument) or change them: {"endpoint","model","apiKey","localModel","device"}.
    std::string configure(const std::string& settingsJson);

protected:
    void onContextReady() override;
    LogosShutdown aboutToUnload() override;

private:
    std::string state(long long since, bool takeViewAction);

    std::unique_ptr<DynamicBus> m_bus;
    std::unique_ptr<Voice> m_voice;
    std::unique_ptr<Engine> m_engine;
};
