#include "planner.h"

#include <cctype>
#include <vector>

using Json = nlohmann::json;

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (std::isspace(static_cast<unsigned char>(s[b])))) ++b;
    while (e > b && (std::isspace(static_cast<unsigned char>(s[e - 1])) || s[e - 1] == '.' || s[e - 1] == '!' || s[e - 1] == '?')) --e;
    return s.substr(b, e - b);
}

bool eat(std::string& s, const std::string& prefix) {
    if (lower(s).compare(0, prefix.size(), prefix) != 0) return false;
    s = trim(s.substr(prefix.size()));
    return true;
}

// "42" -> 42, "true" -> true, "\"x\"" -> "x", anything else -> the text.
Json argValue(const std::string& raw) {
    const std::string t = trim(raw);
    Json j = Json::parse(t, nullptr, false);
    return j.is_discarded() ? Json(t) : j;
}

Json step(const std::string& tool, Json args) { return {{"tool", tool}, {"args", std::move(args)}}; }

Json error(const std::string& msg) { return {{"ok", false}, {"error", msg}}; }

}  // namespace

Json RulePlanner::planOne(const std::string& command) {
    std::string s = trim(command);
    for (const char* p : {"please ", "can you ", "could you ", "i want to ", "i'd like to ", "i would like to "}) eat(s, p);
    const std::string l = lower(s);

    if (l == "status" || l == "what is running" || l == "what's running")
        return step("status", Json::object());
    if (l == "list installed" || l == "what is installed" || l == "what's installed" || l == "installed")
        return step("list_installed", Json::object());
    if (l == "list available" || l == "what can i install")
        return step("list_available", Json::object());
    if (eat(s, "list available ") || eat(s, "search for ") || eat(s, "search ") || eat(s, "find "))
        return step("list_available", {{"query", s}});
    if (eat(s, "methods of ") || eat(s, "methods "))
        return step("module_methods", {{"module", s}});
    if (eat(s, "call ")) {
        // module.method(a, b)  or  module.method a b
        const size_t dot = s.find('.');
        if (dot == std::string::npos) return error("Say it as module.method, for example eth_rpc_module.list_chains.");
        const std::string module = trim(s.substr(0, dot));
        std::string rest = s.substr(dot + 1);
        std::string method;
        Json args = Json::array();
        const size_t paren = rest.find('(');
        if (paren != std::string::npos) {
            method = trim(rest.substr(0, paren));
            const size_t close = rest.rfind(')');
            const std::string inside = rest.substr(paren + 1, close == std::string::npos ? std::string::npos : close - paren - 1);
            std::string cur;
            int depth = 0;
            bool quoted = false;
            for (char c : inside) {
                if (c == '"') quoted = !quoted;
                if (!quoted && (c == '[' || c == '{')) ++depth;
                if (!quoted && (c == ']' || c == '}')) --depth;
                if (c == ',' && depth == 0 && !quoted) { args.push_back(argValue(cur)); cur.clear(); continue; }
                cur += c;
            }
            if (!trim(cur).empty()) args.push_back(argValue(cur));
        } else {
            const size_t sp = rest.find(' ');
            method = trim(rest.substr(0, sp));
            if (sp != std::string::npos) {
                std::string word;
                for (char c : rest.substr(sp + 1) + " ") {
                    if (c == ' ') { if (!word.empty()) args.push_back(argValue(word)); word.clear(); }
                    else word += c;
                }
            }
        }
        if (module.empty() || method.empty()) return error("Say it as module.method, for example eth_rpc_module.list_chains.");
        return step("call", {{"module", module}, {"method", method}, {"args", args}});
    }
    for (const char* p : {"install and open ", "install and run ", "install and start ", "install and launch "})
        if (eat(s, p)) return step("open_app", {{"app", s}});
    for (const char* p : {"open ", "start ", "run ", "launch "})
        if (eat(s, p)) return step("open_app", {{"app", s}});
    if (eat(s, "install ")) return step("install", {{"name", s}});
    return error("I can't plan \"" + trim(command) + "\" yet. Try: open <app>, install <package>, "
                 "search <words>, what is installed, status, methods <module>, call <module>.<method>(args).");
}

Json RulePlanner::plan(const std::string& text) {
    std::vector<std::string> parts;
    std::string rest = text;
    for (;;) {
        const size_t at = lower(rest).find(" then ");
        if (at == std::string::npos) { parts.push_back(rest); break; }
        parts.push_back(rest.substr(0, at));
        rest = rest.substr(at + 6);
    }
    Json steps = Json::array();
    for (const auto& part : parts) {
        if (trim(part).empty()) continue;
        Json s = planOne(part);
        if (s.contains("ok") && !s["ok"].get<bool>()) return s;
        steps.push_back(s);
    }
    if (steps.empty()) return error("I didn't hear a command.");
    return {{"ok", true}, {"steps", steps}};
}
