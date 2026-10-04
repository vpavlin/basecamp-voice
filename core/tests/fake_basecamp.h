#pragma once

// A small stand-in for the parts of Basecamp v0.3.1 the core talks to:
// package_downloader, package_manager, modules_state, and any modules with
// methods. Shapes follow what the spike observed (FINDINGS.md).

#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "bus.h"

using Json = nlohmann::json;

class FakeBasecamp : public Bus {
public:
    struct Pkg { std::string type; long long size; std::vector<std::string> deps; std::string description = ""; };
    std::map<std::string, Pkg> catalog;
    std::set<std::string> installed;
    std::set<std::string> ready;
    std::vector<std::string> installOrder;
    std::map<std::string, Json> methods;                                   // module -> getPluginMethods
    std::map<std::string, std::function<Json(const Json&)>> handlers;     // "module.method"
    std::map<std::string, std::string> downloadErrors;                    // name -> error row
    std::vector<std::string> calls;
    std::mutex mu;
    std::map<std::string, std::function<void(const Json&)>> subscribers;   // "module.event"

    struct Sub : Subscription {
        FakeBasecamp* bc; std::string key;
        ~Sub() override { std::lock_guard<std::mutex> lk(bc->mu); bc->subscribers.erase(key); }
    };
    std::unique_ptr<Subscription> subscribe(const std::string& module, const std::string& event,
                                            std::function<void(const Json&)> cb) override {
        std::lock_guard<std::mutex> lk(mu);
        auto s = std::make_unique<Sub>();
        s->bc = this; s->key = module + "." + event;
        subscribers[s->key] = cb;
        return s;
    }

    FakeBasecamp() {
        catalog["eth_rpc_module"] = {"core", 30LL * 1024 * 1024, {}};
        catalog["eth_rpc_ui"] = {"ui_qml", 14LL * 1024 * 1024, {"eth_rpc_module"}};
        catalog["blockchain_module"] = {"core", 148LL * 1024 * 1024, {}};
        catalog["blockchain_ui"] = {"ui_qml", 13LL * 1024 * 1024, {"blockchain_module"}};
        catalog["lez_wallet_ui"] = {"ui_qml", 12LL * 1024 * 1024, {}};
        catalog["monero_wallet_ui"] = {"ui_qml", 14LL * 1024 * 1024, {}};
        catalog["token_list_module"] = {"core", 29LL * 1024 * 1024, {}};
        catalog["token_list_ui"] = {"ui_qml", 14LL * 1024 * 1024, {"token_list_module"}};
        installed = {"capability_module", "package_manager", "package_downloader", "modules_state"};
        ready = {"capability_module", "package_manager", "package_downloader", "modules_state"};
    }

    int count(const std::string& key) {
        std::lock_guard<std::mutex> lk(mu);
        return static_cast<int>(std::count(calls.begin(), calls.end(), key));
    }

    // What installing `name` adds, dependencies first.
    std::vector<std::string> closure(const std::string& name) {
        std::vector<std::string> out;
        std::function<void(const std::string&)> add = [&](const std::string& n) {
            if (installed.count(n) || std::find(out.begin(), out.end(), n) != out.end()) return;
            for (const auto& d : catalog[n].deps) add(d);
            out.push_back(n);
        };
        add(name);
        return out;
    }

    CallResult invoke(const std::string& module, const std::string& method,
                      const Json& args, int) override {
        std::unique_lock<std::mutex> lk(mu);
        const std::string key = module + "." + method;
        calls.push_back(key);
        CallResult r;
        r.ok = true;
        if (key == "package_downloader.getCatalog") {
            Json list = Json::array();
            for (const auto& [n, p] : catalog) {
                Json deps = Json::array();
                for (const auto& d : p.deps) deps.push_back({{"name", d}, {"version", ">=0.1.0"}});
                list.push_back({{"name", n}, {"type", p.type}, {"description", p.description.empty() ? n + " package" : p.description},
                                {"versions", Json::array({{{"version", "0.1.0"}, {"size", p.size},
                                    {"manifest", {{"name", n}, {"version", "0.1.0"}, {"dependencies", deps}}}}})}});
            }
            r.value = list;
        } else if (key == "package_manager.getInstalledPackages") {
            Json list = Json::array();
            for (const auto& n : installed) {
                Json deps = Json::array();
                if (catalog.count(n)) for (const auto& d : catalog[n].deps) deps.push_back(d);
                list.push_back({{"name", n}, {"type", catalog.count(n) ? catalog[n].type : "core"},
                                {"version", "0.1.0"}, {"dependencies", deps}});
            }
            r.value = list;
        } else if (key == "package_downloader.resolveDependencies" || key == "package_downloader.downloadResolvedDependencies") {
            const Json req = Json::parse(args[0].get<std::string>());
            const bool download = method == "downloadResolvedDependencies";
            if (download && subscribers.count("package_downloader.downloadProgress")) {
                auto cb = subscribers["package_downloader.downloadProgress"];
                for (const auto& n : closure(Json::parse(args[0].get<std::string>())[0]["name"].get<std::string>()))
                    cb(Json::array({n, catalog[n].size, catalog[n].size}));
            }
            Json rows = Json::array();
            for (const auto& n : closure(req[0]["name"].get<std::string>())) {
                Json row = {{"name", n}, {"version", "0.1.0"}, {"rootHash", "h-" + n}};
                if (download && downloadErrors.count(n)) row["error"] = downloadErrors[n];
                else if (download) { row["path"] = "/tmp/lgpd/" + n + ".lgx"; row["source"] = "https://x/" + n; }
                rows.push_back(row);
            }
            r.value = rows;
        } else if (key == "package_manager.installPlugin") {
            std::string path = args[0].get<std::string>();
            std::string n = path.substr(path.rfind('/') + 1);
            n = n.substr(0, n.size() - 4);
            installed.insert(n);
            installOrder.push_back(n);
            r.value = {{"name", n + "-0.1.0"}, {"path", "/ud/" + n}, {"isCoreModule", catalog[n].type == "core"}};
        } else if (key == "modules_state.is_ready") {
            r.value = ready.count(args[0].get<std::string>()) > 0;
        } else if (key == "modules_state.list_modules") {
            Json mods = Json::array();
            for (const auto& n : installed) mods.push_back({{"module", n}, {"state", ready.count(n) ? "ready" : "unloaded"}});
            r.value = {{"modules", mods}, {"partial", false}};
        } else if (method == "getPluginMethods" && methods.count(module)) {
            if (!ready.count(module)) return notRunning(module);
            r.value = methods[module];
        } else if (handlers.count(key)) {
            if (!ready.count(module)) return notRunning(module);
            auto h = handlers[key];
            lk.unlock();
            r.value = h(args);
        } else if (ready.count(module)) {
            r.value = nullptr;   // a misspelled method: ok with null, as in v0.3.1
        } else {
            return notRunning(module);
        }
        return r;
    }

private:
    static CallResult notRunning(const std::string& module) {
        CallResult r;
        r.error = module + " is not running";
        return r;
    }
};
