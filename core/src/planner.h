#pragma once

#include <string>
#include <nlohmann/json.hpp>

// Turns what the user said into tool steps:
//   {"ok": true, "steps": [{"tool": ..., "args": {...}}, ...]}
//   {"ok": false, "error": "<a sentence for the user>"}
// Called on the engine's worker thread; may take seconds (a model).
class Planner {
public:
    virtual ~Planner() = default;
    virtual nlohmann::json plan(const std::string& text) = 0;

    // A later round of the same request: `done` is what has run so far,
    // [{"step": description, "result": summary}]. Same answer shape as plan();
    // no steps means the request is complete (reply says so).
    virtual nlohmann::json planNext(const std::string& text, const nlohmann::json& done) {
        (void)text; (void)done;
        return {{"ok", true}, {"steps", nlohmann::json::array()}, {"reply", ""}};
    }
    // Whether planNext is worth asking (a model can; fixed phrases cannot).
    virtual bool continues() const { return false; }
};

// A stand-in until the model planner lands: a handful of fixed phrasings, so
// the tools, the confirmation and the view can be exercised end to end.
// "then" chains commands: "open eth rpc then call eth_rpc_module.list_chains".
class RulePlanner : public Planner {
public:
    nlohmann::json plan(const std::string& text) override;
    static nlohmann::json planOne(const std::string& command);
};
