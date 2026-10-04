#pragma once

#include <map>
#include <string>
#include <vector>

// Recipes: what an app's own buttons do, written down so Basecamp Voice can do
// it too (docs/recipes.md). An app's buttons call its C++ backend, which is not
// reachable from other modules, so the module calls it makes are recorded here
// as small actions the core runs step by step; the model only chooses one.
struct RecipeFact {
    std::string name;
    std::string file;        // "Logos/BlockchainUI" -> ~/.config/Logos/BlockchainUI.conf
    std::string key;
    bool mustExist = false;  // the value is a path that has to exist
};

struct RecipeStep {
    std::string kind;        // "require" | "skip_if" | "call"
    std::string module, method;
    std::vector<std::string> args;   // raw: $name or a JSON literal
    std::string as;          // call: keep the result as $as
    int timeoutSec = 120;
    std::string fact;        // require: which fact
    std::string message;     // require / skip_if: what to tell the user
};

struct RecipeAction {
    std::string id, title;
    std::vector<RecipeStep> steps;
    // A failing call whose error contains `first` is reported as `second`:
    // what the user can do about it, instead of the module's raw message.
    std::vector<std::pair<std::string, std::string>> errorHints;
};

struct Recipe {
    std::string app, module;
    std::vector<std::string> words;
    std::vector<RecipeFact> facts;
    std::vector<RecipeAction> actions;
    std::vector<std::string> notes;

    const RecipeAction* action(const std::string& id) const;
};

namespace recipes {

// Parses one recipe; sets error and returns false on a malformed one.
bool parse(const std::string& text, Recipe* out, std::string* error);
// The recipes compiled in (core/recipes/*.inc).
const std::vector<Recipe>& all();
const Recipe* forApp(const std::string& app);
// Facts that have a value right now; a must-exist path that is gone is absent.
// home: the user's home directory.
std::map<std::string, std::string> resolveFacts(const Recipe& r, const std::string& home);
// A Qt INI settings value ([General] section or none), unquoted. Empty if absent.
std::string iniValue(const std::string& path, const std::string& key);

}  // namespace recipes
