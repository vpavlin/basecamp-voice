R"RECIPE(
# Storage node (storage_ui, storage_module)
#
# From storage_module's own documentation (Basecamp 0.3.1): init(cfg) takes the
# whole configuration and saves it, so the saved one is loaded first
# (loadConfigOrDefault migrates it, or returns defaults on a first run) and
# passed back unchanged; start() is accepted at once and the node comes up in
# the background.

app: storage_ui
module: storage_module
words: storage, codex, files, upload, download

action start_node: Start the storage node with its saved settings
  skip_if storage_module.isRunning(): The storage node is already running.
  call storage_module.loadConfigOrDefault() as cfg
  call storage_module.init($cfg) timeout 120
  call storage_module.start() timeout 120

action stop_node: Stop the storage node
  call storage_module.stop() timeout 120
)RECIPE"
