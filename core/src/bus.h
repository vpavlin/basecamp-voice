#pragma once

#include <functional>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>

// The one seam between the core and the rest of Basecamp: a by-name call to a
// module. The module wires it to modules().dynamic(); tests wire a fake.
struct CallResult {
    bool ok = false;
    nlohmann::json value;   // the result when ok
    std::string error;      // a sentence when not ok
};

// Keeps an event subscription alive; unsubscribes when destroyed.
class Subscription {
public:
    virtual ~Subscription() = default;
};

class Bus {
public:
    virtual ~Bus() = default;
    virtual CallResult invoke(const std::string& module, const std::string& method,
                              const nlohmann::json& args, int timeoutMs) = 0;
    // `cb` gets the event's arguments as a JSON array, possibly on another
    // thread. Null when the bus cannot subscribe.
    virtual std::unique_ptr<Subscription> subscribe(const std::string& module, const std::string& event,
                                                    std::function<void(const nlohmann::json& args)> cb) {
        (void)module; (void)event; (void)cb;
        return nullptr;
    }
};
