{
  description = "basecamp_voice_core - plans and runs spoken Basecamp commands";

  inputs = {
    # The builder rev Basecamp v0.3.1's own package_downloader was built with:
    # same cpp-sdk / logos-protocol as the host (docs/adr/0001).
    logos-module-builder.url = "github:logos-co/logos-module-builder/4b7998272c5ec014bcac4bf1c7dbe7602c63a3c1";
    logos-module-builder.inputs.logos-standalone-app.follows = "";
    # Parakeet speech-to-text, built from a pinned whisper.cpp (nix/stt).
    stt.url = "path:../nix/stt";
    stt.inputs.nixpkgs.follows = "logos-module-builder/nixpkgs";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
      externalLibInputs = {
        voicestt = inputs.stt;
      };
    };
}
