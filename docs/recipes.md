# Recipes: doing what an app's buttons do

## Why recipes

Asked to "start the blockchain node", the model could open the Blockchain app
but not start the node. The node starts with a configuration the app chose
during its setup, and nothing in Basecamp tells the model which.

Three ways to press an app's Start button were considered (2026-10-04):

| Way | Result |
|---|---|
| Read the app's QML for the calls its buttons make | Works only for pure-QML apps. The official apps (blockchain_ui, eth_rpc_ui, token_list_ui) have a C++ backend: the button calls `backend.startBlockchain()`, and what that does is compiled code. |
| Call the app backend's action (`startBlockchain`) by name from our core | **Tested, does not work in v0.3.1.** The backend receives a token so that it can call modules, but it is not published as something others can call. `modules_state` does not list it, and `blockchain_ui.getTimeInfo()` failed with `object_unavailable` after the full timeout. |
| Write down what the button does: a **recipe** | Chosen. |

The proper long-term fix is **app intents**: apps declare actions such as
`blockchain.node.start` in `provides`, and Basecamp Voice raises them through
the shell. *Update:* Basecamp 0.3.1 already supports this, and Basecamp Voice
uses it (ADR 0009). Scala, the calendar, is the first app that provides intents.
An app that does needs no recipe; recipes remain for apps that do not.

## What a recipe is

A small text file per app, `core/recipes/<name>.inc`, compiled into the core.
It declares **actions** that the core runs step by step. The model only picks
one with the `recipe {app, action}` tool. The user sees the exact calls, with
the real values, in the plan before pressing **Do it**.

Running an action:
1. If the recipe's module is not running, its app is opened (that loads the
   module).
2. The steps run in order. A failing step stops the action and is reported.

The model sees the recipes for apps that are installed or mentioned, with
whether each action can run:

```
Recipes (they do what the app's own buttons do; use them when they fit):
- blockchain_ui: start_node (Start the blockchain node; ready), stop_node (Stop the blockchain node; ready)
  Note: The Blockchain app shows the node it started itself; ...
```

## Format

```
R"RECIPE(
# Comments start with #. Say where the knowledge comes from.

app: blockchain_ui                       # the app (ui_qml package)
module: blockchain_module                # the module its buttons call
words: blockchain, node, chain           # words that bring it into the model's context

fact config_path: setting Logos/BlockchainUI userConfigPath must-exist
fact deployment: setting Logos/BlockchainUI deploymentConfigPath

action start_node: Start the blockchain node
  require config_path: The node has not been set up yet. Open the Blockchain app ...
  call blockchain_module.start($config_path, $deployment) timeout 900

note: A sentence the model should know when this recipe is shown.
)RECIPE"
```

- **`fact <name>: setting <Org/App> <key> [must-exist]`:** a value read from
  the app's own Qt settings file, `~/.config/<Org>/<App>.conf`, key `<key>`.
  - Only files under `~/.config/Logos/` are read, never anything else.
  - `must-exist`: the value is a path, and a path that no longer exists counts
    as missing.
- **Steps**, indented under an action:
  - **`require <fact>: <message>`:** the action cannot run without that fact.
    The message tells the user what to do, and it is shown instead of a plan.
  - **`skip_if <module>.<method>(<args>): <message>`:** if the call returns
    true (or `{value: true}`), the action stops there, successfully, with the
    message (e.g. "already running").
  - **`call <module>.<method>(<args>) [timeout <s>] [as <name>]`:**
    - Arguments are `$fact`, `$name` (an earlier `as`) or a JSON literal
      (`""`, `0`, `true`, `{"a":1}`).
    - `as` keeps the result for later steps. A `{success, value}` result keeps
      its `value`.
    - The default timeout is 120 s.
  - **`when_error "<text>": <message>`:** if a call fails with an error
    containing `<text>`, the user gets `<message>`, which says what they can
    do about it, followed by the shortened raw error.
- **The parser checks every `$name`** and every `require`'s fact. A recipe
  that does not parse is skipped, and the reason is printed to the log.

Add a recipe: write the file, then:
1. add it to `core/recipes/all.inc` and `core/CMakeLists.txt`;
2. add a case to `core/eval/cases.json`;
3. run the unit tests: `the_shipped_recipes_parse` counts them.

## The recipes

### Blockchain node: `blockchain.inc`

Source: logos-blockchain-ui 0.3.1, `BlockchainBackend::startBlockchain`.
- **What the app does:** Start calls
  `blockchain_module.start(userConfig, deploymentConfig)`.
  - `userConfig` is the path the app saved after its setup, in
    `~/.config/Logos/BlockchainUI.conf`, key `userConfigPath`.
  - An empty `deploymentConfig` means the deployment built into the module.
- **The setup itself is not imitated:** keys, wallet and config are the app's
  onboarding. Without a saved config the action says so and sends the user to
  the app.
- **Actions:**
  - `start_node`: `start($config_path, $deployment)`, 15 min timeout (the node
    replays its backlog).
  - `stop_node`: `stop()`.
- **Advice on known failures** (`when_error`). A config saved by an older
  module version fails with "Unrecognized fields" (seen 2026-10-04 on a config
  from August). The app has an **Update config** button for this:
  `migrate_user_config`, `merge_user_config`, then backup and swap, next to the
  keystore. The recipe does not repeat it, because it moves files beside the
  user's keys. It tells the user to use the app's button.
- **Known gap:** the app tracks the node it started itself. A node started from
  here may show as stopped in the app until the app refreshes.

### Storage node: `storage.inc`

Source: storage_module's own method documentation (Basecamp 0.3.1).
- **The order matters.** `init(cfg)` takes the whole configuration and saves
  it to `~/.logos_storage/config.json`. Calling it with defaults would
  overwrite the user's settings, so:
  1. `isRunning()`: if true, stop ("already running").
  2. `loadConfigOrDefault()`, which migrates the saved configuration or
     returns defaults on a first run, kept as `$cfg`.
  3. `init($cfg)`.
  4. `start()`: accepted at once; the node comes up in the background.
- **Actions:** `start_node` (the steps above) and `stop_node` (`stop()`).

## Safety

- **Every recipe action needs the user's confirmation,** like any call.
- **Facts read only Logos settings files,** with no other paths and no `..`.
- **Recipes are compiled in.** Nothing downloaded or written by another module
  can add one.
