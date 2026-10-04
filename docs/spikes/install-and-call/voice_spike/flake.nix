{
  description = "voice_spike - answers the basecamp-voice open questions";

  inputs = {
    # The builder rev Basecamp v0.3.1's own package_downloader was built with
    # (cpp-sdk 3f34c0b, logos-protocol 8bbc027 - same protocol as the host).
    logos-module-builder.url = "github:logos-co/logos-module-builder/4b7998272c5ec014bcac4bf1c7dbe7602c63a3c1";
    logos-module-builder.inputs.logos-standalone-app.follows = "";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
