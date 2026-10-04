{
  description = "voice_spike_ui - spike view";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder/4b7998272c5ec014bcac4bf1c7dbe7602c63a3c1";
    logos-module-builder.inputs.logos-standalone-app.follows = "";
    voice_spike.url = "path:../voice_spike";
    voice_spike.inputs.logos-module-builder.follows = "logos-module-builder";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
