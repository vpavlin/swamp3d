{
  description = "Swamp core module: signed 3D-model catalogue over loam_core + files in Logos Storage (Basecamp 0.3).";

  inputs = {
    # Basecamp 0.3 stack (docs/adr/0012): builder 0.3.1, loam_core port/0.3 (upstream delivery 0.3.0),
    # storage_module 3.0.1 (the host owns the node at runtime).
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.3.1";
    loam_core.url = "github:vpavlin/loam-basecamp/5db9069d7b953b5876210576393b1dc9ffc19fdf?dir=core";
    storage_module.url = "github:logos-co/logos-storage-module/v3.0.1";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
