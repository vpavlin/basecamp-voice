#pragma once

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "planner.h"

// Plans with a language model (docs/adr/0006): a fixed system prompt
// (system_prompt.inc), the facts of this moment (Tools::context) and a JSON
// schema the server enforces (plan_schema.inc), so the model can only answer
// with the tools. Its "reply" is the sentence shown to the user.
class ModelPlanner : public Planner {
public:
    // Context for what was said (installed, running, catalog matches, methods).
    using Context = std::function<std::string(const std::string& text)>;
    // One chat completion constrained by the schema; fills the reply text.
    // stats: what the server reported for this request (tokens, ms), or {}.
    using Chat = std::function<bool(const nlohmann::json& messages, const nlohmann::json& schema,
                                    std::string* content, std::string* error, nlohmann::json* stats)>;

    // The conversation so far (Engine::history), or {} for none.
    using History = std::function<nlohmann::json()>;

    ModelPlanner(Context context, Chat chat, History history = {});

    // The "Conversation so far" block the model reads (mirrored by eval.py).
    static std::string historyText(const nlohmann::json& history);

    nlohmann::json plan(const std::string& text) override;
    nlohmann::json planNext(const std::string& text, const nlohmann::json& done) override;
    bool continues() const override { return true; }

    static const char* systemPrompt();
    static const nlohmann::json& schema();
    // The model's JSON -> {"ok":true,"steps":[...],"reply":"..."} or {"ok":false,"error":...}.
    static nlohmann::json parse(const std::string& content);

private:
    nlohmann::json ask(const std::string& userMessage);

    Context m_context;
    History m_history;
    Chat m_chat;
};
