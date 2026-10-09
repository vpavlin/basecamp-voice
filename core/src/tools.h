#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "bus.h"

using Json = nlohmann::json;

// A plan step after preparation: names resolved against the catalog and the
// installed set, and described in one line the user can confirm.
struct Prepared {
    bool ok = false;
    std::string error;
    Json step;                 // {"tool": ..., "args": {...}} with names resolved
    std::string description;
    bool mutating = false;     // changes something: needs the user's confirmation
};

// The tool set the planner may use (docs/adr/0004-tool-set.md). Nothing reaches
// Basecamp except through these, and every call goes through the checks here:
// the target must be running, the method must exist with that many arguments,
// and an {"ok":false} inside a result counts as a failure.
//
// Not thread-safe: the engine calls it from its one worker thread.
// An intent an installed app provides (its metadata.json "provides"), that
// Basecamp Voice may raise (listed in its own "uses"). docs/adr/0009-app-intents.md
struct AppIntent {
    struct Param { std::string name, type, description; bool required = false; };
    std::string app, intent, description;
    bool readOnly = false;   // the provider's word that it changes nothing
    bool handoff = false;    // the provider keeps the user (Basecamp does not return them)
    std::vector<Param> params;
};

class Tools {
public:
    using OpenApp = std::function<CallResult(const std::string& app)>;
    using Progress = std::function<void(const std::string& message)>;

    Tools(Bus& bus, OpenApp openApp);

    static const std::vector<std::string>& names();

    Prepared prepare(const Json& step);

    // What the planner needs to know right now, as text: installed apps and
    // modules, what is running, catalog entries matching the words said, and
    // the methods of running modules. Mirrored by core/eval/eval.py.
    std::string context(const std::string& text);
    // On success the value carries a "summary" sentence.
    CallResult run(const Json& step, const Progress& progress);

    // Checked while waiting for a module to start, so stopping the engine
    // (module unload) does not sit out the whole wait.
    std::function<bool()> stopping = [] { return false; };

    // Where recipe facts are read from (~/.config/Logos/...). Tests point it elsewhere.
    std::string homeDir;

    // Raises an intent through the view (logos.request); the answer is
    // Basecamp's envelope {ok, data, error}. Unset: intents cannot run.
    std::function<CallResult(const std::string& intent, const Json& params)> raiseIntent;
    // The app whose "uses" says which intents may be raised.
    std::string selfApp = "basecamp_voice";
    // What the installed apps provide that this app may raise.
    std::vector<AppIntent> intents();
    // Each look for intents reports what it found and, if nothing, why:
    // {"count", "problem"?, "self", "selfDir", "uses", "apps": [{app, type, dir, metadata, provides, usable}]}.
    std::function<void(const Json&)> onIntentReport;
    // The last intents() could not read the package list (Basecamp not ready yet).
    bool packagesUnavailable = false;
    // "Now: Sunday 2026-10-04 14:05 (local time)": what "tomorrow at 3" is measured from.
    std::function<std::string()> nowText;

    int callTimeoutMs = 120000;
    int installTimeoutMs = 300000;
    int readyWaitMs = 60000;
    int readyPollMs = 500;

    // Results often arrive as a JSON string, sometimes encoded twice.
    static Json unwrap(const Json& value);
    // {"ok":false}, {"success":false} or a non-empty "error" inside a result.
    static bool innerFailure(const Json& value, std::string* error);
    static bool isReadOnlyMethod(const std::string& method);
    // Lowercase letters and digits only: "Eth RPC_ui" -> "ethrpcui".
    static std::string normalize(const std::string& text);

private:
    struct Package {
        std::string name;
        std::string type;
        std::string displayName;
        std::string description;
        std::string version;
        long long size = 0;
        std::vector<std::string> dependencies;
        bool installed = false;
        std::string installDir;
        Json raw;   // our own entry from getInstalledPackages, for the intents report
    };

    Prepared prepareInstall(const Json& args);
    Prepared prepareOpenApp(const Json& args);
    Prepared prepareCall(const Json& args);
    Prepared prepareRecipe(const Json& args);
    Prepared prepareIntent(const Json& args);

    CallResult runListInstalled();
    CallResult runListAvailable(const Json& args);
    CallResult runInstall(const std::string& name, const Progress& progress);
    CallResult runOpenApp(const Json& args, const Progress& progress);
    CallResult runModuleMethods(const Json& args);
    CallResult runCall(const Json& args);
    CallResult runStatus();
    CallResult runRecipe(const Json& args, const Progress& progress);
    CallResult runAddRepository(const Json& args);
    CallResult runIntent(const Json& args);

    CallResult invoke(const std::string& module, const std::string& method,
                      const Json& args, int timeoutMs);
    CallResult requireReady(const std::string& module);
    CallResult methodsOf(const std::string& module, bool refresh);

    // The catalog joined with what is installed, keyed by package name.
    bool loadPackages(std::map<std::string, Package>* out, std::string* error);
    std::vector<AppIntent> intentsOf(const std::map<std::string, Package>& packages) const;
    Json installedForResolver(const std::map<std::string, Package>& packages) const;
    // Resolve a spoken or typed name. forApps: only ui_qml packages, and a
    // core's name finds the app that depends on it.
    bool resolve(const std::string& query, bool forApps,
                 const std::map<std::string, Package>& packages,
                 std::string* name, std::string* error) const;
    // What installing `name` would add: {names in order, total bytes}.
    bool previewInstall(const std::string& name, const std::map<std::string, Package>& packages,
                        std::vector<std::string>* names, long long* bytes, std::string* error);

    Bus& m_bus;
    OpenApp m_openApp;
    Json m_catalog;
    long long m_catalogAt = 0;
    std::map<std::string, Json> m_methods;
};
