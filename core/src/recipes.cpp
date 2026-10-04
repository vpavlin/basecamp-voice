#include "recipes.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool startsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }

// "module.method(a, b) rest" -> module, method, args, rest
bool parseCall(const std::string& text, RecipeStep* st, std::string* rest) {
    const size_t dot = text.find('.'), open = text.find('('), close = text.rfind(')');
    if (dot == std::string::npos || open == std::string::npos || close == std::string::npos || !(dot < open && open < close))
        return false;
    st->module = trim(text.substr(0, dot));
    st->method = trim(text.substr(dot + 1, open - dot - 1));
    const std::string inside = text.substr(open + 1, close - open - 1);
    std::string cur;
    int depth = 0;
    bool quoted = false;
    for (char c : inside) {
        if (c == '"') quoted = !quoted;
        if (!quoted && (c == '{' || c == '[')) ++depth;
        if (!quoted && (c == '}' || c == ']')) --depth;
        if (c == ',' && depth == 0 && !quoted) { st->args.push_back(trim(cur)); cur.clear(); continue; }
        cur += c;
    }
    if (!trim(cur).empty()) st->args.push_back(trim(cur));
    *rest = trim(text.substr(close + 1));
    return !st->module.empty() && !st->method.empty();
}

const char* kAll[] = {
#include "../recipes/all.inc"
};

}  // namespace

const RecipeAction* Recipe::action(const std::string& id) const {
    for (const auto& a : actions) if (a.id == id) return &a;
    return nullptr;
}

namespace recipes {

bool parse(const std::string& text, Recipe* out, std::string* error) {
    Recipe r;
    RecipeAction* current = nullptr;
    std::istringstream in(text);
    std::string raw;
    int lineNo = 0;
    auto fail = [&](const std::string& why) { *error = "line " + std::to_string(lineNo) + ": " + why; return false; };
    while (std::getline(in, raw)) {
        ++lineNo;
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        const bool indented = !raw.empty() && std::isspace(static_cast<unsigned char>(raw[0]));
        if (indented) {
            if (!current) return fail("a step outside an action");
            RecipeStep st;
            if (startsWith(line, "when_error ")) {
                // when_error "<text in the error>": <message for the user>
                const size_t q1 = line.find('"'), q2 = line.find('"', q1 == std::string::npos ? 0 : q1 + 1);
                const size_t colon = q2 == std::string::npos ? std::string::npos : line.find(':', q2);
                if (q1 == std::string::npos || q2 == std::string::npos || colon == std::string::npos)
                    return fail("when_error needs \"text\": message");
                current->errorHints.emplace_back(line.substr(q1 + 1, q2 - q1 - 1), trim(line.substr(colon + 1)));
                continue;
            }
            if (startsWith(line, "require ")) {
                const size_t colon = line.find(':');
                if (colon == std::string::npos) return fail("require needs ': message'");
                st.kind = "require";
                st.fact = trim(line.substr(8, colon - 8));
                st.message = trim(line.substr(colon + 1));
            } else if (startsWith(line, "skip_if ")) {
                const size_t close = line.find(')');
                const size_t colon = line.find(':', close == std::string::npos ? 0 : close);
                std::string rest;
                if (colon == std::string::npos || !parseCall(line.substr(8, colon - 8), &st, &rest))
                    return fail("skip_if needs module.method(): message");
                st.kind = "skip_if";
                st.message = trim(line.substr(colon + 1));
            } else if (startsWith(line, "call ")) {
                std::string rest;
                if (!parseCall(line.substr(5), &st, &rest)) return fail("call needs module.method(args)");
                st.kind = "call";
                std::istringstream words(rest);
                std::string w;
                while (words >> w) {
                    if (w == "timeout") { if (!(words >> st.timeoutSec)) return fail("timeout needs seconds"); }
                    else if (w == "as") { if (!(words >> st.as)) return fail("as needs a name"); }
                    else return fail("unknown word after the call: " + w);
                }
            } else {
                return fail("unknown step: " + line);
            }
            current->steps.push_back(st);
            continue;
        }
        current = nullptr;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) return fail("expected 'key: value'");
        const std::string key = trim(line.substr(0, colon)), value = trim(line.substr(colon + 1));
        if (key == "app") r.app = value;
        else if (key == "module") r.module = value;
        else if (key == "note") r.notes.push_back(value);
        else if (key == "words") {
            std::string w;
            std::istringstream ws(value);
            while (std::getline(ws, w, ',')) if (!trim(w).empty()) r.words.push_back(trim(w));
        } else if (startsWith(key, "fact ")) {
            RecipeFact f;
            f.name = trim(key.substr(5));
            std::istringstream fs(value);
            std::string kind, flag;
            fs >> kind >> f.file >> f.key;
            if (kind != "setting" || f.file.empty() || f.key.empty()) return fail("fact: setting <Org/App> <key> [must-exist]");
            if (fs >> flag) { if (flag != "must-exist") return fail("unknown fact flag " + flag); f.mustExist = true; }
            r.facts.push_back(f);
        } else if (startsWith(key, "action ")) {
            RecipeAction a;
            a.id = trim(key.substr(7));
            a.title = value;
            r.actions.push_back(a);
            current = &r.actions.back();
        } else {
            return fail("unknown key " + key);
        }
    }
    if (r.app.empty() || r.module.empty()) { *error = "a recipe needs app: and module:"; return false; }
    // Every $name used must be a fact or an earlier "as".
    for (const auto& a : r.actions) {
        std::vector<std::string> known;
        for (const auto& f : r.facts) known.push_back(f.name);
        for (const auto& st : a.steps) {
            for (const auto& arg : st.args)
                if (!arg.empty() && arg[0] == '$' && std::find(known.begin(), known.end(), arg.substr(1)) == known.end()) {
                    *error = a.id + ": $" + arg.substr(1) + " is not a fact or an earlier result";
                    return false;
                }
            if (st.kind == "require" && std::find(known.begin(), known.end(), st.fact) == known.end()) {
                *error = a.id + ": require of an unknown fact " + st.fact;
                return false;
            }
            if (!st.as.empty()) known.push_back(st.as);
        }
    }
    *out = r;
    return true;
}

const std::vector<Recipe>& all() {
    static const std::vector<Recipe> list = [] {
        std::vector<Recipe> v;
        for (const char* text : kAll) {
            Recipe r;
            std::string err;
            if (parse(text, &r, &err)) v.push_back(r);
            else fprintf(stderr, "basecamp_voice_core: a recipe does not parse: %s\n", err.c_str());
        }
        return v;
    }();
    return list;
}

const Recipe* forApp(const std::string& app) {
    for (const auto& r : all()) if (r.app == app) return &r;
    return nullptr;
}

std::string iniValue(const std::string& path, const std::string& key) {
    std::ifstream f(path);
    std::string line, section;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') { section = line; continue; }
        if (!section.empty() && section != "[General]") continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos || trim(line.substr(0, eq)) != key) continue;
        std::string v = trim(line.substr(eq + 1));
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        return v;
    }
    return {};
}

std::map<std::string, std::string> resolveFacts(const Recipe& r, const std::string& home) {
    std::map<std::string, std::string> out;
    for (const auto& f : r.facts) {
        // Only Logos settings: ~/.config/Logos/<App>.conf, nothing else.
        if (!startsWith(f.file, "Logos/") || f.file.find("..") != std::string::npos) continue;
        const std::string v = iniValue(home + "/.config/" + f.file + ".conf", f.key);
        if (f.mustExist) {
            struct stat st;
            if (v.empty() || ::stat(v.c_str(), &st) != 0) continue;
        }
        out[f.name] = v;
    }
    return out;
}

}  // namespace recipes
