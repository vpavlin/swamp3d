{
  description = "Swamp desktop view: pure-QML over the swamp_core module (Basecamp 0.3)";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.3.1";
    swamp_core.url = "path:../swamp_core";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
