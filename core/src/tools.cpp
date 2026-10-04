#include "tools.h"
#include "text.h"
#include "recipes.h"

#include <cstdlib>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <set>
#include <sstream>
#include <mutex>
#include <thread>

namespace {

long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string str(const Json& j, const char* key) {
    if (!j.is_object() || !j.contains(key) || !j[key].is_string()) return {};
    return j[key].get<std::string>();
}

std::string sizeText(long long bytes) {
    if (bytes <= 0) return {};
    std::ostringstream s;
    if (bytes >= 1024 * 1024) s << (bytes + 512 * 1024) / (1024 * 1024) << " MB";
    else s << (bytes + 512) / 1024 << " KB";
    return s.str();
}

std::string joinNames(const std::vector<std::string>& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += (i + 1 == v.size()) ? " and " : ", ";
        out += v[i];
    }
    return out;
}

// Dependencies come as names or {"name": ...} objects.
std::vector<std::string> depNames(const Json& deps) {
    std::vector<std::string> out;
    if (!deps.is_array()) return out;
    for (const auto& d : deps) {
        if (d.is_string()) out.push_back(d.get<std::string>());
        else if (d.is_object() && d.contains("name") && d["name"].is_string())
            out.push_back(d["name"].get<std::string>());
    }
    return out;
}

CallResult fail(const std::string& error) {
    CallResult r;
    r.error = error;
    return r;
}

CallResult done(Json value) {
    CallResult r;
    r.ok = true;
    r.value = std::move(value);
    return r;
}

// Words that say what kind of thing was asked for, not which one.
const std::set<std::string>& fillerWords() {
    static const std::set<std::string> words = {
        "the", "a", "an", "app", "application", "module", "ui", "plugin", "package", "basecamp"};
    return words;
}

std::string queryKey(const std::string& query, bool dropKindWords) {
    std::string key, word;
    auto flush = [&]() {
        if (word.empty()) return;
        if (!(dropKindWords ? fillerWords().count(word) : (word == "the" || word == "a" || word == "an")))
            key += word;
        word.clear();
    };
    for (char c : query) {
        if (std::isalnum(static_cast<unsigned char>(c))) word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else flush();
    }
    flush();
    return key;
}

}  // namespace

Tools::Tools(Bus& bus, OpenApp openApp) : m_bus(bus), m_openApp(std::move(openApp)) {
    const char* h = std::getenv("HOME");
    homeDir = h ? h : "";
}

const std::vector<std::string>& Tools::names() {
    static const std::vector<std::string> n = {
        "list_installed", "list_available", "install", "open_app", "module_methods", "call", "status", "recipe",
        "add_repository"};
    return n;
}

Json Tools::unwrap(const Json& value) {
    Json v = value;
    for (int i = 0; i < 2 && v.is_string(); ++i) {
        Json parsed = Json::parse(v.get<std::string>(), nullptr, false);
        if (parsed.is_discarded()) break;
        v = parsed;
    }
    return v;
}

bool Tools::innerFailure(const Json& value, std::string* error) {
    if (!value.is_object()) return false;
    const bool okFalse = value.contains("ok") && value["ok"].is_boolean() && !value["ok"].get<bool>();
    const bool successFalse = value.contains("success") && value["success"].is_boolean() && !value["success"].get<bool>();
    std::string err;
    if (value.contains("error")) err = value["error"].is_string() ? value["error"].get<std::string>() : safeDump(value["error"]);
    if (okFalse || successFalse || !err.empty()) {
        if (error) *error = err.empty() ? "the module reported a failure without a reason" : err;
        return true;
    }
    return false;
}

bool Tools::isReadOnlyMethod(const std::string& method) {
    static const std::vector<std::string> prefixes = {
        "get", "list", "is", "has", "describe", "status", "version", "count", "find", "search", "show"};
    std::string m;
    for (char c : method) m += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const auto& p : prefixes) {
        if (m.compare(0, p.size(), p) != 0) continue;
        // "is_ready", "isReady", "getX", "list" - but not "issue" or "listen".
        if (m.size() == p.size()) return true;
        const char next = method[p.size()];
        if (next == '_' || std::isupper(static_cast<unsigned char>(next))) return true;
    }
    return false;
}

std::string Tools::normalize(const std::string& text) {
    std::string out;
    for (char c : text)
        if (std::isalnum(static_cast<unsigned char>(c)))
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// ---- calls ---------------------------------------------------------------

CallResult Tools::invoke(const std::string& module, const std::string& method,
                         const Json& args, int timeoutMs) {
    CallResult r = m_bus.invoke(module, method, args, timeoutMs);
    if (!r.ok) return r;
    r.value = unwrap(r.value);
    std::string err;
    if (innerFailure(r.value, &err)) return fail(module + "." + method + ": " + err);
    return r;
}

CallResult Tools::requireReady(const std::string& module) {
    // An unloaded target does not fail fast: a call blocks for its whole
    // timeout. modules_state answers in about a millisecond (FINDINGS.md, Q1).
    CallResult r = invoke("modules_state", "is_ready", Json::array({module}), 5000);
    if (!r.ok) return fail("Could not check whether " + module + " is running: " + r.error);
    if (!(r.value.is_boolean() && r.value.get<bool>()))
        return fail(module + " is not running. Open its app first.");
    return done(true);
}

CallResult Tools::methodsOf(const std::string& module, bool refresh) {
    auto it = m_methods.find(module);
    if (it != m_methods.end() && !refresh) return done(it->second);
    CallResult r = invoke(module, "getPluginMethods", Json::array(), 10000);
    if (!r.ok) return r;
    if (!r.value.is_array()) return fail(module + " did not describe its methods");
    m_methods[module] = r.value;
    return r;
}

// ---- packages ------------------------------------------------------------

bool Tools::loadPackages(std::map<std::string, Package>* out, std::string* error) {
    if (m_catalog.is_null() || nowMs() - m_catalogAt > 60000) {
        CallResult c = invoke("package_downloader", "getCatalog", Json::array(), 60000);
        if (!c.ok) { *error = "Could not read the package catalog: " + c.error; return false; }
        m_catalog = c.value;
        m_catalogAt = nowMs();
    }
    CallResult inst = invoke("package_manager", "getInstalledPackages", Json::array(), 10000);
    if (!inst.ok) { *error = "Could not list installed packages: " + inst.error; return false; }

    out->clear();
    if (m_catalog.is_array()) {
        for (const auto& e : m_catalog) {
            Package p;
            p.name = str(e, "name");
            if (p.name.empty()) continue;
            p.type = str(e, "type");
            p.description = str(e, "description");
            p.displayName = str(e, "displayName");
            if (e.contains("versions") && e["versions"].is_array() && !e["versions"].empty()) {
                const Json& v = e["versions"][0];
                const Json manifest = v.contains("manifest") ? v["manifest"] : Json::object();
                p.version = str(v, "version").empty() ? str(manifest, "version") : str(v, "version");
                if (v.contains("size") && v["size"].is_number()) p.size = v["size"].get<long long>();
                if (p.displayName.empty()) p.displayName = str(manifest, "display_name");
                if (manifest.contains("dependencies")) p.dependencies = depNames(manifest["dependencies"]);
            }
            (*out)[p.name] = p;
        }
    }
    if (inst.value.is_array()) {
        for (const auto& e : inst.value) {
            const std::string name = str(e, "name");
            if (name.empty()) continue;
            Package& p = (*out)[name];
            p.name = name;
            p.installed = true;
            if (p.type.empty()) p.type = str(e, "type");
            if (p.description.empty()) p.description = str(e, "description");
            if (p.displayName.empty()) p.displayName = str(e, "displayName");
            p.version = str(e, "version");
            // The installed manifest is what Basecamp will load.
            if (e.contains("dependencies")) p.dependencies = depNames(e["dependencies"]);
        }
    }
    return true;
}

Json Tools::installedForResolver(const std::map<std::string, Package>& packages) const {
    Json out = Json::array();
    for (const auto& [name, p] : packages)
        if (p.installed) out.push_back({{"name", name}, {"version", p.version}});
    return out;
}

bool Tools::resolve(const std::string& query, bool forApps,
                    const std::map<std::string, Package>& packages,
                    std::string* name, std::string* error) const {
    if (packages.count(query) && (!forApps || packages.at(query).type == "ui_qml")) {
        *name = query;
        return true;
    }
    auto isApp = [](const Package& p) { return p.type == "ui_qml"; };

    // First the words as spoken ("token list module" is a module), then
    // without the kind words ("the eth rpc app" -> "ethrpc").
    for (bool dropKind : {false, true}) {
        const std::string key = queryKey(query, dropKind);
        if (key.empty()) continue;
        std::set<std::string> exact, partial;
        for (const auto& [n, p] : packages) {
            const std::string nn = normalize(n), dn = normalize(p.displayName);
            const bool isExact = nn == key || dn == key || nn == key + "ui" || nn == key + "module";
            const bool isPartial = nn.find(key) != std::string::npos || (!dn.empty() && dn.find(key) != std::string::npos);
            if (!isExact && !isPartial) continue;
            if (forApps && !isApp(p)) {
                // A core asked for by name: the apps that depend on it.
                for (const auto& [an, ap] : packages)
                    if (isApp(ap) && std::find(ap.dependencies.begin(), ap.dependencies.end(), n) != ap.dependencies.end())
                        (isExact ? exact : partial).insert(an);
                continue;
            }
            (isExact ? exact : partial).insert(n);
        }
        const std::set<std::string>& pick = !exact.empty() ? exact : partial;
        if (pick.size() == 1) { *name = *pick.begin(); return true; }
        if (pick.size() > 1) {
            std::vector<std::string> v(pick.begin(), pick.end());
            if (v.size() > 8) v.resize(8);
            *error = "Several " + std::string(forApps ? "apps" : "packages") + " match \"" + query + "\": " + joinNames(v) + ". Which one?";
            return false;
        }
    }
    *error = std::string("No ") + (forApps ? "app" : "package") + " called \"" + query + "\" is installed or in the catalog.";
    return false;
}

bool Tools::previewInstall(const std::string& name, const std::map<std::string, Package>& packages,
                           std::vector<std::string>* names, long long* bytes, std::string* error) {
    const Json deps = Json::array({Json{{"name", name}}});
    CallResult r = invoke("package_downloader", "resolveDependencies",
                          Json::array({safeDump(deps), safeDump(installedForResolver(packages))}), 60000);
    if (!r.ok) { *error = "Could not work out what installing " + name + " needs: " + r.error; return false; }
    names->clear();
    *bytes = 0;
    if (r.value.is_array()) {
        for (const auto& row : r.value) {
            const std::string n = str(row, "name");
            if (n.empty()) continue;
            if (row.contains("error")) { *error = "Cannot install " + n + ": " + str(row, "error"); return false; }
            names->push_back(n);
            auto it = packages.find(n);
            if (it != packages.end()) *bytes += it->second.size;
        }
    }
    if (names->empty()) { *error = "The catalog has nothing to install for " + name + "."; return false; }
    return true;
}

// ---- prepare -------------------------------------------------------------

Prepared Tools::prepare(const Json& step) {
    Prepared p;
    const std::string tool = str(step, "tool");
    const Json args = step.contains("args") && step["args"].is_object() ? step["args"] : Json::object();
    if (std::find(names().begin(), names().end(), tool) == names().end()) {
        p.error = "Unknown tool \"" + tool + "\"";
        return p;
    }
    if (tool == "install") return prepareInstall(args);
    if (tool == "open_app") return prepareOpenApp(args);
    if (tool == "call") return prepareCall(args);
    if (tool == "recipe") return prepareRecipe(args);

    p.ok = true;
    p.step = {{"tool", tool}, {"args", args}};
    if (tool == "list_installed") p.description = "List the installed packages";
    else if (tool == "status") p.description = "Check which modules are running";
    else if (tool == "list_available") {
        const std::string q = str(args, "query"), repo = str(args, "repository");
        p.description = !repo.empty() ? "List the packages in " + repo
                      : q.empty() ? "List the packages in the catalog" : "Search the catalog for \"" + q + "\"";
    } else if (tool == "add_repository") {
        const std::string url = str(args, "url");
        // A repository decides what can be installed: always confirmed, and
        // only over https (or plain http on this machine).
        if (url.rfind("https://", 0) != 0 && url.rfind("http://127.0.0.1", 0) != 0 && url.rfind("http://localhost", 0) != 0)
            return Prepared{false, "A package repository has to be an https:// address (its logos-repo.json)."};
        p.mutating = true;
        p.description = "Add the package repository " + url + " (its packages may be unsigned: install only what you trust)";
    } else if (tool == "module_methods") {
        const std::string m = str(args, "module");
        if (m.empty()) return Prepared{false, "module_methods needs a module name"};
        p.description = "List what " + m + " can do";
    }
    return p;
}

Prepared Tools::prepareInstall(const Json& args) {
    Prepared p;
    std::map<std::string, Package> packages;
    std::string name;
    if (!loadPackages(&packages, &p.error)) return p;
    if (!resolve(str(args, "name"), false, packages, &name, &p.error)) return p;

    p.ok = true;
    if (packages[name].installed) {
        p.step = {{"tool", "install"}, {"args", {{"name", name}, {"alreadyInstalled", true}}}};
        p.description = name + " is already installed";
        return p;
    }
    std::vector<std::string> adds;
    long long bytes = 0;
    if (!previewInstall(name, packages, &adds, &bytes, &p.error)) { p.ok = false; return p; }
    p.mutating = true;
    p.step = {{"tool", "install"}, {"args", {{"name", name}}}};
    std::vector<std::string> others;
    for (const auto& n : adds) if (n != name) others.push_back(n);
    p.description = "Install " + name;
    if (!packages[name].version.empty()) p.description += " " + packages[name].version;
    if (!others.empty()) p.description += " with " + joinNames(others);
    if (bytes > 0) p.description += " (" + sizeText(bytes) + ")";
    return p;
}

Prepared Tools::prepareOpenApp(const Json& args) {
    Prepared p;
    std::map<std::string, Package> packages;
    std::string app;
    if (!loadPackages(&packages, &p.error)) return p;
    if (!resolve(str(args, "app"), true, packages, &app, &p.error)) return p;

    p.ok = true;
    p.mutating = true;
    const bool install = !packages[app].installed;
    p.step = {{"tool", "open_app"}, {"args", {{"app", app}, {"install", install}}}};
    if (!install) {
        p.description = "Open " + app;
        return p;
    }
    std::vector<std::string> adds;
    long long bytes = 0;
    if (!previewInstall(app, packages, &adds, &bytes, &p.error)) { p.ok = false; return p; }
    std::vector<std::string> others;
    for (const auto& n : adds) if (n != app) others.push_back(n);
    p.description = "Install " + app;
    if (!others.empty()) p.description += " with " + joinNames(others);
    if (bytes > 0) p.description += " (" + sizeText(bytes) + ")";
    p.description += ", then open it";
    return p;
}

Prepared Tools::prepareCall(const Json& args) {
    Prepared p;
    const std::string module = str(args, "module"), method = str(args, "method");
    if (module.empty() || method.empty()) { p.error = "call needs a module and a method"; return p; }
    // Basecamp's own modules are reached through the tools (install, status),
    // never by a raw call; and nothing calls back into this module.
    static const std::set<std::string> offLimits = {"capability_module", "package_manager", "package_downloader",
                                                    "modules_state", "basecamp_voice_core"};
    if (offLimits.count(module)) { p.error = "Calling " + module + " directly is not allowed."; return p; }
    Json a = args.contains("args") && args["args"].is_array() ? args["args"] : Json::array();
    p.ok = true;
    // Every call is confirmed by the user. A method's name says nothing
    // reliable about what it does, and the plan may come from text a stranger
    // wrote (a package description in the model's context). The target may
    // only start during this plan (open_app first), so the method itself is
    // checked when the step runs.
    p.mutating = true;
    p.step = {{"tool", "call"}, {"args", {{"module", module}, {"method", method}, {"args", a}}}};
    std::string shown;
    for (size_t i = 0; i < a.size(); ++i) shown += (i ? ", " : "") + (a[i].is_string() ? a[i].get<std::string>() : safeDump(a[i]));
    p.description = "Call " + module + "." + method + "(" + shown + ")";
    return p;
}

// ---- run -----------------------------------------------------------------

CallResult Tools::run(const Json& step, const Progress& progress) {
    const std::string tool = str(step, "tool");
    const Json args = step.contains("args") && step["args"].is_object() ? step["args"] : Json::object();
    if (tool == "list_installed") return runListInstalled();
    if (tool == "list_available") return runListAvailable(args);
    if (tool == "add_repository") return runAddRepository(args);
    if (tool == "install") {
        if (args.value("alreadyInstalled", false))
            return done({{"name", str(args, "name")}, {"summary", str(args, "name") + " was already installed."}});
        return runInstall(str(args, "name"), progress);
    }
    if (tool == "open_app") return runOpenApp(args, progress);
    if (tool == "module_methods") return runModuleMethods(args);
    if (tool == "call") return runCall(args);
    if (tool == "status") return runStatus();
    if (tool == "recipe") return runRecipe(args, progress);
    return fail("Unknown tool \"" + tool + "\"");
}

// Up to `max` names, then "and N more": a summary that says what was found.
static std::string nameList(const std::vector<std::string>& v, size_t max = 12) {
    std::string out;
    for (size_t i = 0; i < v.size() && i < max; ++i) out += (i ? ", " : "") + v[i];
    if (v.size() > max) out += " and " + std::to_string(v.size() - max) + " more";
    return out;
}

// A listing: the items for the view, a short summary for the user, and the
// names in one line for the planner's next round.
// The model gets the same numbered list the user sees, so "the third one"
// means the same thing to both.
static CallResult listing(Json value, const Json& items, const std::string& summary, const std::string& header) {
    std::string numbered;
    for (size_t i = 0; i < items.size() && i < 40; ++i) {
        const std::string note = items[i].value("note", std::string());
        numbered += (i ? ", " : "") + std::to_string(i + 1) + ". " + items[i].value("name", std::string()) +
                    (note.empty() ? "" : " (" + note + ")");
    }
    if (items.size() > 40) numbered += " and " + std::to_string(items.size() - 40) + " more";
    value["items"] = items;
    value["summary"] = summary;
    value["forModel"] = header + ": " + numbered + ".";
    CallResult r;
    r.ok = true;
    r.value = std::move(value);
    return r;
}

static Json item(const std::string& name, const std::string& note) { return {{"name", name}, {"note", note}}; }

static bool isSystemModule(const std::string& n) {
    return n == "capability_module" || n == "package_manager" || n == "package_downloader" ||
           n == "modules_state" || n == "basecamp_voice_core" || n == "package_manager_ui" || n == "main_ui";
}

CallResult Tools::runListInstalled() {
    std::map<std::string, Package> packages;
    std::string error;
    if (!loadPackages(&packages, &error)) return fail(error);
    Json list = Json::array();
    for (const auto& [n, p] : packages)
        if (p.installed) list.push_back({{"name", n}, {"type", p.type}, {"version", p.version}});
    std::vector<std::string> names;
    Json items = Json::array(), modules = Json::array();
    for (const auto& [n, p] : packages) {
        if (!p.installed || isSystemModule(n)) continue;
        names.push_back(n + (p.type == "ui_qml" ? " (app)" : ""));
        (p.type == "ui_qml" ? items : modules).push_back(item(n, std::string(p.type == "ui_qml" ? "app" : "module") + (p.version.empty() ? "" : " " + p.version)));
    }
    for (const auto& m : modules) items.push_back(m);
    if (names.empty()) return done({{"packages", list}, {"summary", "Nothing is installed besides Basecamp itself."}});
    return listing({{"packages", list}}, items, std::to_string(names.size()) + " packages are installed.",
                   "Installed");
}

CallResult Tools::runAddRepository(const Json& args) {
    const std::string url = str(args, "url");
    CallResult r = invoke("package_downloader", "addRepository", Json::array({url}), 60000);
    if (!r.ok) return fail("Could not add the repository: " + r.error);
    m_catalogAt = 0;   // the next catalog read includes it
    return done({{"url", url}, {"summary", "Added the repository " + url + "."}});
}

CallResult Tools::runListAvailable(const Json& args) {
    // One repository: its own catalog, exactly as published.
    const std::string repo = str(args, "repository");
    if (!repo.empty()) {
        CallResult c = invoke("package_downloader", "getCatalogForRepo", Json::array({repo}), 60000);
        if (!c.ok) return fail("Could not read " + repo + ": " + c.error);
        std::vector<std::string> names;
        Json list = Json::array(), apps = Json::array(), modules = Json::array();
        if (c.value.is_array())
            for (const auto& e : c.value) {
                const std::string n = str(e, "name");
                if (n.empty()) continue;
                const bool app = str(e, "type") == "ui_qml";
                names.push_back(n + (app ? " (app)" : ""));
                list.push_back({{"name", n}, {"type", str(e, "type")}, {"description", utf8Prefix(str(e, "description"), 140)}});
                (app ? apps : modules).push_back(item(n, app ? "app" : "module"));
            }
        if (names.empty()) return done({{"packages", list}, {"summary", repo + " has no packages."}});
        for (const auto& m : modules) apps.push_back(m);
        return listing({{"packages", list}}, apps,
                       repo + " has " + std::to_string(names.size()) + " packages.",
                       repo + " has");
    }
    std::map<std::string, Package> packages;
    std::string error;
    if (!loadPackages(&packages, &error)) return fail(error);
    const std::string key = queryKey(str(args, "query"), true);
    Json list = Json::array();
    for (const auto& [n, p] : packages) {
        if (!key.empty() && normalize(n + p.displayName + p.description).find(key) == std::string::npos) continue;
        Json e = {{"name", n}, {"type", p.type}, {"version", p.version}, {"installed", p.installed},
                  {"description", utf8Prefix(p.description, 140)}};
        if (p.size > 0) e["size"] = sizeText(p.size);
        list.push_back(e);
        if (list.size() >= 30) break;
    }
    std::vector<std::string> names;
    Json items = Json::array();
    for (const auto& e : list) {
        std::string n = e["name"].get<std::string>(), tags = e["type"] == "ui_qml" ? "app" : "module";
        if (e["installed"].get<bool>()) tags += ", installed";
        if (e.contains("size")) tags += ", " + e["size"].get<std::string>();
        names.push_back(n + " (" + tags + ")");
        items.push_back(item(n, tags));
    }
    if (names.empty()) return done({{"packages", list}, {"summary", "Nothing in the catalog matches."}});
    return listing({{"packages", list}}, items, std::to_string(names.size()) + " packages match.",
                   "Found");
}

CallResult Tools::runInstall(const std::string& name, const Progress& progress) {
    std::map<std::string, Package> packages;
    std::string error;
    if (!loadPackages(&packages, &error)) return fail(error);
    if (packages.count(name) && packages[name].installed)
        return done({{"installed", Json::array()}, {"summary", name + " was already installed."}});

    progress("Downloading " + name + " and what it needs");
    // package_downloader reports bytes as they arrive: show them, at most
    // twice a second, so a 148 MB module does not look stuck.
    struct Last { std::mutex mu; long long at = 0; };
    auto last = std::make_shared<Last>();
    auto sub = m_bus.subscribe("package_downloader", "downloadProgress", [progress, last](const Json& a) {
        if (!a.is_array() || a.size() < 3 || !a[0].is_string() || !a[1].is_number() || !a[2].is_number()) return;
        const long long got = a[1].get<long long>(), total = a[2].get<long long>();
        {
            std::lock_guard<std::mutex> lk(last->mu);
            const long long now = nowMs();
            if (now - last->at < 500 && got < total) return;
            last->at = now;
        }
        progress("Downloading " + a[0].get<std::string>() + ": " + sizeText(got) +
                 (total > 0 ? " of " + sizeText(total) : std::string()));
    });
    const Json deps = Json::array({Json{{"name", name}}});
    CallResult dl = invoke("package_downloader", "downloadResolvedDependencies",
                           Json::array({safeDump(deps), safeDump(installedForResolver(packages))}), installTimeoutMs);
    if (!dl.ok) return fail("Download failed: " + dl.error);
    if (!dl.value.is_array() || dl.value.empty()) return fail("Nothing was downloaded for " + name + ".");

    std::vector<std::string> installed;
    for (const auto& row : dl.value) {
        const std::string n = str(row, "name");
        if (row.contains("error")) return fail("Could not download " + n + ": " + str(row, "error"));
        const std::string path = str(row, "path");
        if (path.empty()) return fail("The download of " + n + " has no file.");
        progress("Installing " + n);
        CallResult in = invoke("package_manager", "installPlugin",
                               Json::array({path, false, str(row, "source")}), installTimeoutMs);
        if (!in.ok) return fail("Could not install " + n + ": " + in.error);
        installed.push_back(n);
    }
    if (!loadPackages(&packages, &error)) return fail(error);
    if (!packages.count(name) || !packages[name].installed)
        return fail("Basecamp does not list " + name + " as installed after installing it.");
    return done({{"installed", installed}, {"summary", "Installed " + joinNames(installed) + "."}});
}

CallResult Tools::runOpenApp(const Json& args, const Progress& progress) {
    const std::string app = str(args, "app");
    std::string summary;
    if (args.value("install", false)) {
        CallResult in = runInstall(app, progress);
        if (!in.ok) return in;
        summary = in.value.value("summary", "") + " ";
    }
    std::map<std::string, Package> packages;
    std::string error;
    if (!loadPackages(&packages, &error)) return fail(error);
    if (!packages.count(app) || !packages[app].installed) return fail(app + " is not installed.");

    progress("Opening " + app);
    CallResult opened = m_openApp(app);
    if (!opened.ok) return fail("Could not open " + app + ": " + opened.error);

    // Opening the app makes Basecamp load its cores; wait until they answer.
    std::vector<std::string> cores;
    for (const auto& d : packages[app].dependencies)
        if (!packages.count(d) || packages[d].type != "ui_qml") cores.push_back(d);
    for (const auto& core : cores) {
        progress("Waiting for " + core + " to start");
        const long long deadline = nowMs() + readyWaitMs;
        for (;;) {
            CallResult r = invoke("modules_state", "is_ready", Json::array({core}), 5000);
            if (r.ok && r.value.is_boolean() && r.value.get<bool>()) break;
            if (stopping()) return fail("Stopped while waiting for " + core + " to start.");
            if (nowMs() >= deadline)
                return fail(app + " opened, but " + core + " did not start within " + std::to_string(readyWaitMs / 1000) + " s.");
            std::this_thread::sleep_for(std::chrono::milliseconds(readyPollMs));
        }
    }
    summary += "Opened " + app + ".";
    if (!cores.empty()) summary += " " + joinNames(cores) + (cores.size() == 1 ? " is" : " are") + " running.";
    return done({{"app", app}, {"running", cores}, {"summary", summary}});
}

CallResult Tools::runModuleMethods(const Json& args) {
    const std::string module = str(args, "module");
    CallResult ready = requireReady(module);
    if (!ready.ok) return ready;
    CallResult m = methodsOf(module, true);
    if (!m.ok) return m;
    Json list = Json::array();
    for (const auto& e : m.value) {
        Json params = Json::array();
        if (e.contains("parameters") && e["parameters"].is_array())
            for (const auto& prm : e["parameters"]) params.push_back(str(prm, "name") + ": " + str(prm, "type"));
        list.push_back({{"name", str(e, "name")}, {"parameters", params}, {"returns", str(e, "returnType")}});
    }
    std::vector<std::string> sigs;
    for (const auto& e : list) {
        std::string ps;
        for (const auto& prm : e["parameters"]) {
            const std::string full = prm.get<std::string>();
            ps += (ps.empty() ? "" : ", ") + full.substr(0, full.find(':'));
        }
        sigs.push_back(e["name"].get<std::string>() + "(" + ps + ")");
    }
    Json items = Json::array();
    for (size_t i = 0; i < sigs.size(); ++i) items.push_back(item(sigs[i], str(list[i], "returns")));
    return listing({{"module", module}, {"methods", list}}, items,
                   module + " has " + std::to_string(sigs.size()) + " methods.",
                   module + " can");
}

CallResult Tools::runCall(const Json& args) {
    const std::string module = str(args, "module"), method = str(args, "method");
    const Json a = args.contains("args") && args["args"].is_array() ? args["args"] : Json::array();
    CallResult ready = requireReady(module);
    if (!ready.ok) return ready;

    // A misspelled method returns ok with null (FINDINGS.md), so check first.
    std::vector<size_t> arities;
    for (bool refresh : {false, true}) {
        CallResult m = methodsOf(module, refresh);
        if (!m.ok) return m;
        arities.clear();
        for (const auto& e : m.value)
            if (str(e, "name") == method)
                arities.push_back(e.contains("parameters") && e["parameters"].is_array() ? e["parameters"].size() : 0);
        if (!arities.empty()) break;
    }
    if (arities.empty()) return fail(module + " has no method called " + method + ".");
    if (std::find(arities.begin(), arities.end(), a.size()) == arities.end())
        return fail(module + "." + method + " takes " + std::to_string(arities.front()) + " arguments, not " + std::to_string(a.size()) + ".");

    CallResult r = invoke(module, method, a, callTimeoutMs);
    if (!r.ok) return r;
    std::string shown = r.value.is_string() ? r.value.get<std::string>() : safeDump(r.value);
    if (shown.size() > 300) shown = utf8Prefix(shown, 300) + "...";
    return done({{"module", module}, {"method", method}, {"result", r.value},
                 {"summary", module + "." + method + " returned " + shown}});
}

CallResult Tools::runStatus() {
    CallResult r = invoke("modules_state", "list_modules", Json::array(), 10000);
    if (!r.ok) return r;
    Json running = Json::array();
    const Json mods = r.value.contains("modules") ? r.value["modules"] : Json::array();
    for (const auto& m : mods) {
        const std::string state = str(m, "state");
        if (state == "ready" || state == "loaded")
            running.push_back(str(m, "module"));
    }
    std::vector<std::string> names;
    for (const auto& n : running) if (!isSystemModule(n.get<std::string>())) names.push_back(n.get<std::string>());
    if (names.empty())
        return done({{"running", running}, {"modules", mods}, {"summary", "No modules are running besides Basecamp's own."}});
    Json items = Json::array();
    for (const auto& n : names) items.push_back(item(n, "running"));
    return listing({{"running", running}, {"modules", mods}}, items,
                   std::to_string(names.size()) + " modules are running.", "Running");
}

// ---- context for the planner ---------------------------------------------------
//
// Kept identical to context() in core/eval/eval.py, which is what the model
// choice and the prompt were measured with (docs/model-eval.md).

namespace {

const std::set<std::string>& systemModules() {
    static const std::set<std::string> s = {"capability_module", "package_manager", "package_downloader",
                                            "modules_state", "basecamp_voice_core"};
    return s;
}

// Words that say what to do, not to what: they would match half the catalog.
const std::set<std::string>& stopwords() {
    static const std::set<std::string> s = {
        "the", "and", "app", "apps", "application", "open", "start", "run", "launch", "install", "please",
        "can", "could", "you", "what", "which", "show", "list", "module", "modules", "for", "with", "this",
        "that", "there", "are", "have", "like", "want", "need", "get", "set", "thing", "about", "into",
        "from", "all", "any", "some", "now", "right", "uh", "um"};
    return s;
}

const size_t kMaxMatches = 8;
const size_t kDescChars = 70;

std::string commaJoin(const std::vector<std::string>& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) out += (i ? ", " : "") + v[i];
    return out;
}

}  // namespace

std::string Tools::context(const std::string& text) {
    std::vector<std::string> words;
    {
        std::string w;
        for (char c : text + " ") {
            const unsigned char u = static_cast<unsigned char>(c);
            if ((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9')) w += c;
            else if (u >= 'A' && u <= 'Z') w += static_cast<char>(u - 'A' + 'a');
            else { if (w.size() >= 3 && !stopwords().count(w)) words.push_back(w); w.clear(); }
        }
    }
    std::map<std::string, Package> packages;
    std::string error;
    std::string out = "Context:\n";
    const bool havePackages = loadPackages(&packages, &error);
    if (!havePackages) out += "(The package list is unavailable: " + error + ")\n";

    std::vector<std::string> apps, mods;
    for (const auto& [n, p] : packages) {
        if (!p.installed || systemModules().count(n)) continue;
        if (p.type == "ui_qml") apps.push_back(n + (p.dependencies.empty() ? "" : " (needs " + commaJoin(p.dependencies) + ")"));
        else mods.push_back(n);
    }
    std::vector<std::string> running;
    CallResult st = invoke("modules_state", "list_modules", Json::array(), 5000);
    if (st.ok && st.value.is_object() && st.value.contains("modules") && st.value["modules"].is_array())
        for (const auto& m : st.value["modules"]) {
            const std::string s = str(m, "state");
            if ((s == "ready" || s == "loaded") && !systemModules().count(str(m, "module"))) running.push_back(str(m, "module"));
        }
    std::sort(running.begin(), running.end());
    out += "Installed apps: " + (apps.empty() ? std::string("none") : commaJoin(apps)) + "\n";
    out += "Installed modules: " + (mods.empty() ? std::string("none") : commaJoin(mods)) + "\n";
    out += "Running modules: " + (running.empty() ? std::string("none") : commaJoin(running)) + "\n";

    if (havePackages) {
        // Every app by name, so a request in any language can be mapped.
        std::vector<std::string> allApps;
        for (const auto& [n, p] : packages)
            if (p.type == "ui_qml" && !systemModules().count(n)) allApps.push_back(n);
        out += "Apps in the catalog: " + commaJoin(allApps) + "\n";

        // Catalog entries whose name or description contains a word that was said.
        std::vector<std::string> matches;
        for (const auto& [n, p] : packages) {
            if (systemModules().count(n)) continue;
            const std::string hay = normalize(n + " " + p.description);
            for (const auto& w : words)
                if (hay.find(w) != std::string::npos) { matches.push_back(n); break; }
        }
        std::stable_sort(matches.begin(), matches.end(), [&](const std::string& a, const std::string& b) {
            return (packages[a].type == "ui_qml") > (packages[b].type == "ui_qml");
        });
        if (matches.size() > kMaxMatches) matches.resize(kMaxMatches);
        if (!matches.empty()) {
            out += "Catalog matches:\n";
            for (const auto& n : matches) {
                const Package& p = packages[n];
                const bool app = p.type == "ui_qml";
                out += "- " + n + " (" + (app ? "app" : "module") + ")";
                if (app && !p.dependencies.empty()) out += " needs " + commaJoin(p.dependencies);
                out += ": " + utf8Prefix(p.description, kDescChars);
                if (p.installed) out += " [installed]";
                out += "\n";
            }
        }
    }
    // Recipes for the apps here or mentioned: what the app's own buttons do.
    {
        std::string lines;
        for (const auto& r : recipes::all()) {
            const bool installed = packages.count(r.app) && packages[r.app].installed;
            bool mentioned = false;
            for (const auto& w : words)
                for (const auto& rw : r.words) mentioned = mentioned || normalize(rw).find(w) != std::string::npos || w.find(normalize(rw)) != std::string::npos;
            if (!installed && !mentioned) continue;
            const auto facts = recipes::resolveFacts(r, homeDir);
            std::vector<std::string> acts;
            for (const auto& a : r.actions) {
                std::string ready = "ready";
                for (const auto& st : a.steps)
                    if (st.kind == "require" && !facts.count(st.fact)) ready = "not set up in the app yet";
                acts.push_back(a.id + " (" + a.title + "; " + ready + ")");
            }
            lines += "- " + r.app + (installed ? "" : " (not installed)") + ": " + commaJoin(acts) + "\n";
            for (const auto& n : r.notes) lines += "  Note: " + n + "\n";
        }
        if (!lines.empty()) out += "Recipes (they do what the app's own buttons do; use them when they fit):\n" + lines;
    }
    // Methods of the running modules, so the model calls only what exists.
    for (const auto& m : running) {
        CallResult r = methodsOf(m, false);
        if (!r.ok || !r.value.is_array()) continue;
        std::vector<std::string> sigs;
        for (const auto& e : r.value) {
            std::vector<std::string> ps;
            if (e.contains("parameters") && e["parameters"].is_array())
                for (const auto& prm : e["parameters"]) ps.push_back(str(prm, "name") + ": " + str(prm, "type"));
            sigs.push_back(str(e, "name") + "(" + commaJoin(ps) + ")");
        }
        if (!sigs.empty()) out += "Methods of " + m + ": " + commaJoin(sigs) + "\n";
    }
    out += "\nUser: \"" + text + "\"";
    return out;
}

// ---- recipes (docs/recipes.md) -------------------------------------------------

namespace {

// "$name" -> the fact or earlier result; anything else is a JSON literal.
bool recipeArg(const std::string& raw, const std::map<std::string, Json>& vars, Json* out, std::string* error) {
    if (!raw.empty() && raw[0] == '$') {
        auto it = vars.find(raw.substr(1));
        if (it == vars.end()) { *error = raw + " has no value"; return false; }
        *out = it->second;
        return true;
    }
    Json j = Json::parse(raw, nullptr, false);
    if (j.is_discarded()) { *error = "not a value: " + raw; return false; }
    *out = j;
    return true;
}

std::string shownArg(const Json& v) {
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        return s.empty() ? std::string("\"\"") : utf8Prefix(s, 80);
    }
    return utf8Prefix(safeDump(v), 80);
}

}  // namespace

Prepared Tools::prepareRecipe(const Json& args) {
    Prepared p;
    std::map<std::string, Package> packages;
    std::string app;
    if (!loadPackages(&packages, &p.error)) return p;
    if (!resolve(str(args, "app"), true, packages, &app, &p.error)) return p;
    const Recipe* r = recipes::forApp(app);
    if (!r) { p.error = "There is no recipe for " + app + "."; return p; }
    const RecipeAction* a = r->action(str(args, "action"));
    if (!a) { p.error = app + " has no action \"" + str(args, "action") + "\"."; return p; }
    if (!packages.count(app) || !packages[app].installed) { p.error = app + " is not installed. Install it first."; return p; }

    const auto facts = recipes::resolveFacts(*r, homeDir);
    std::map<std::string, Json> vars;
    for (const auto& [k, v] : facts) vars[k] = v;
    std::string calls;
    for (const auto& st : a->steps) {
        if (st.kind == "require" && !facts.count(st.fact)) { p.error = st.message; return p; }
        if (st.kind != "call") continue;
        std::string shown;
        for (const auto& raw : st.args) {
            Json v;
            std::string err;
            shown += (shown.empty() ? "" : ", ") + (recipeArg(raw, vars, &v, &err) ? shownArg(v) : raw);
        }
        calls += (calls.empty() ? "" : ", then ") + st.module + "." + st.method + "(" + shown + ")";
    }
    p.ok = true;
    p.mutating = true;
    p.step = {{"tool", "recipe"}, {"args", {{"app", app}, {"action", a->id}}}};
    p.description = a->title + ": " + calls;
    return p;
}

CallResult Tools::runRecipe(const Json& args, const Progress& progress) {
    const Recipe* r = recipes::forApp(str(args, "app"));
    const RecipeAction* a = r ? r->action(str(args, "action")) : nullptr;
    if (!a) return fail("Unknown recipe.");

    // The module answers only while its app is open: open it first if needed.
    CallResult ready = invoke("modules_state", "is_ready", Json::array({r->module}), 5000);
    if (!(ready.ok && ready.value.is_boolean() && ready.value.get<bool>())) {
        CallResult opened = runOpenApp({{"app", r->app}, {"install", false}}, progress);
        if (!opened.ok) return opened;
    }

    std::map<std::string, Json> vars;
    for (const auto& [k, v] : recipes::resolveFacts(*r, homeDir)) vars[k] = v;
    for (const auto& st : a->steps) {
        if (st.kind == "require") {
            if (!vars.count(st.fact)) return fail(st.message);
            continue;
        }
        Json callArgs = Json::array();
        for (const auto& raw : st.args) {
            Json v;
            std::string err;
            if (!recipeArg(raw, vars, &v, &err)) return fail(err);
            callArgs.push_back(v);
        }
        if (st.kind == "skip_if") {
            CallResult c = invoke(st.module, st.method, callArgs, st.timeoutSec * 1000);
            const Json v = c.ok && c.value.is_object() && c.value.contains("value") ? c.value["value"] : c.value;
            if (c.ok && v.is_boolean() && v.get<bool>())
                return done({{"recipe", a->id}, {"summary", st.message}});
            continue;
        }
        progress(st.module + "." + st.method);
        CallResult c = invoke(st.module, st.method, callArgs, st.timeoutSec * 1000);
        if (!c.ok) {
            for (const auto& [text, hint] : a->errorHints)
                if (c.error.find(text) != std::string::npos)
                    return fail(hint + " (" + st.module + "." + st.method + ": " + utf8Prefix(c.error, 160) + ")");
            return fail(a->title + " failed at " + st.module + "." + st.method + ": " + c.error);
        }
        if (!st.as.empty()) {
            // StdLogosResult arrives as {success, value}: keep the value.
            vars[st.as] = c.value.is_object() && c.value.contains("value") ? c.value["value"] : c.value;
        }
    }
    return done({{"recipe", a->id}, {"summary", a->title + ": done."}});
}
