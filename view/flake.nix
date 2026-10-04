{
  description = "basecamp_voice - the Basecamp Voice view";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder/4b7998272c5ec014bcac4bf1c7dbe7602c63a3c1";
    logos-module-builder.inputs.logos-standalone-app.follows = "";
    # The core's LIDL contract; same builder rev as the core.
    basecamp_voice_core.url = "path:../core";
    basecamp_voice_core.inputs.logos-module-builder.follows = "logos-module-builder";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
