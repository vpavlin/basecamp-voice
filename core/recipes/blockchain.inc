R"RECIPE(
# Blockchain node (blockchain_ui, blockchain_module)
#
# What the Blockchain app's Start/Stop buttons do (logos-blockchain-ui 0.3.1,
# BlockchainBackend::startBlockchain): blockchain_module.start(userConfig,
# deploymentConfig), where userConfig is the path the app saved when the node
# was set up and an empty deploymentConfig means the deployment built into the
# module. Setting a node up (keys, wallet, config) is the app's onboarding; we
# do not imitate it.

app: blockchain_ui
module: blockchain_module
words: blockchain, node, chain, cryptarchia, mining, validator

fact config_path: setting Logos/BlockchainUI userConfigPath must-exist
fact deployment: setting Logos/BlockchainUI deploymentConfigPath

action start_node: Start the blockchain node
  require config_path: The node has not been set up yet. Open the Blockchain app and follow its setup (it creates your keys and configuration), then ask again.
  call blockchain_module.start($config_path, $deployment) timeout 900
  when_error "Unrecognized fields": Your node configuration is from an older version of the Blockchain module. Open the Blockchain app and use its Update config (it carries your settings and keys across), then ask again.
  when_error "Could not parse config file": The node configuration could not be read. Open the Blockchain app: it can check and update the configuration.

action stop_node: Stop the blockchain node
  call blockchain_module.stop() timeout 120

note: The Blockchain app shows the node it started itself; a node started here may show as stopped there until the app refreshes.
)RECIPE"
