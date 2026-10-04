#include "model_planner.h"
#include "recipes.h"
#include "text.h"

using Json = nlohmann::json;

namespace {
const char* kSystemPrompt =
#include "system_prompt.inc"
    ;
const char* kSchema =
#include "plan_schema.inc"
    ;
}

ModelPlanner::ModelPlanner(Context context, Chat chat, History history, Intents intents)
    : m_context(std::move(context)), m_history(std::move(history)), m_intents(std::move(intents)), m_chat(std::move(chat)) {}

std::string ModelPlanner::historyText(const Json& history) {
    if (!history.is_array() || history.empty()) return {};
    std::string out = "Conversation so far (oldest first; the user may refer to it, e.g. \"the third one\", \"that app\"):\n";
    for (const auto& h : history) {
        if (!h.is_object()) continue;
        out += "- User: \"" + h.value("said", std::string()) + "\"\n  Result: " + h.value("result", std::string()) + "\n";
        const std::string lists = h.value("lists", std::string());
        if (!lists.empty()) out += "  Listed: " + lists + "\n";
    }
    return out + "\n";
}

const char* ModelPlanner::systemPrompt() { return kSystemPrompt; }

// The base schema, with the recipe tool narrowed to the recipes compiled in:
// the grammar then cannot produce one that does not exist.
static const Json& baseSchema() {
    static const Json s = [] {
        Json j = Json::parse(kSchema);
        Json& any = j["properties"]["steps"]["items"]["anyOf"];
        for (auto it = any.begin(); it != any.end(); ++it) {
            if ((*it)["properties"]["tool"]["const"] != "recipe") continue;
            Json variants = Json::array();
            for (const auto& r : recipes::all()) {
                Json actions = Json::array();
                for (const auto& a : r.actions) actions.push_back(a.id);
                variants.push_back({{"type", "object"},
                                    {"properties", {{"app", {{"const", r.app}}}, {"action", {{"enum", actions}}}}},
                                    {"required", {"app", "action"}}, {"additionalProperties", false}});
            }
            if (variants.empty()) { any.erase(it); break; }
            (*it)["properties"]["args"] = {{"anyOf", variants}};
            break;
        }
        return j;
    }();
    return s;
}

// Likewise the intent tool: one variant per intent, with exactly its
// parameters and their types, so the model cannot name one that is not there.
Json ModelPlanner::schemaFor(const std::vector<AppIntent>& intents) {
    Json j = baseSchema();
    Json& any = j["properties"]["steps"]["items"]["anyOf"];
    for (auto it = any.begin(); it != any.end(); ++it) {
        if ((*it)["properties"]["tool"]["const"] != "intent") continue;
        if (intents.empty()) { any.erase(it); break; }
        Json variants = Json::array();
        for (const auto& a : intents) {
            Json props = Json::object(), required = Json::array();
            for (const auto& p : a.params) {
                const std::string t = p.type == "bool" ? "boolean" : p.type;
                props[p.name] = (t == "string" || t == "number" || t == "boolean" || t == "object" || t == "array")
                                    ? Json{{"type", t}} : Json::object();
                if (p.required) required.push_back(p.name);
            }
            variants.push_back({{"type", "object"},
                                {"properties", {{"intent", {{"const", a.intent}}},
                                                {"params", {{"type", "object"}, {"properties", props}, {"required", required},
                                                            {"additionalProperties", false}}}}},
                                {"required", {"intent", "params"}}, {"additionalProperties", false}});
        }
        (*it)["properties"]["args"] = {{"anyOf", variants}};
        break;
    }
    return j;
}

const Json& ModelPlanner::schema() {
    static const Json s = schemaFor({});
    return s;
}

Json ModelPlanner::parse(const std::string& content) {
    Json j = Json::parse(content, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return {{"ok", false}, {"error", "The model's answer was not a plan."}};
    const std::string reply = j.contains("reply") && j["reply"].is_string() ? j["reply"].get<std::string>() : std::string();
    Json steps = Json::array();
    if (j.contains("steps") && j["steps"].is_array()) {
        for (const auto& s : j["steps"]) {
            if (!s.is_object() || !s.contains("tool") || !s["tool"].is_string()) continue;
            steps.push_back({{"tool", s["tool"]}, {"args", s.contains("args") && s["args"].is_object() ? s["args"] : Json::object()}});
        }
    }
    if (steps.empty() && reply.empty())
        return {{"ok", false}, {"error", "The model did not come up with a plan."}};
    return {{"ok", true}, {"steps", steps}, {"reply", reply}};
}

Json ModelPlanner::plan(const std::string& text) {
    return ask(historyText(m_history ? m_history() : Json::array()) + m_context(text));
}

// The context is rebuilt, so modules opened in the last round now show
// their methods; what already ran is listed so it is not planned again.
Json ModelPlanner::planNext(const std::string& text, const Json& done) {
    std::string msg = historyText(m_history ? m_history() : Json::array()) + m_context(text);
    msg += "\n\nDone so far for this request:\n";
    for (const auto& d : done)
        if (d.is_object())
            // Capped: a long listing is already in front of the user, and
            // re-reading it all made follow-up rounds slow (3,000+ tokens).
            msg += "- " + d.value("step", std::string()) + ": " + utf8Prefix(d.value("result", std::string()), 300) + "\n";
    msg += "\nPlan only what is left of the request. If nothing is left, or you need something from the user "
           "(say what), return no steps and say so in reply.";
    return ask(msg);
}

Json ModelPlanner::ask(const std::string& userMessage) {
    const Json messages = Json::array({
        {{"role", "system"}, {"content", kSystemPrompt}},
        {{"role", "user"}, {"content", userMessage}},
    });
    std::string content, error;
    Json stats = Json::object();
    const Json sch = m_intents ? schemaFor(m_intents()) : schema();
    if (!m_chat(messages, sch, &content, &error, &stats))
        return {{"ok", false}, {"error", error}};
    Json plan = parse(content);
    plan["stats"] = stats;
    return plan;
}
