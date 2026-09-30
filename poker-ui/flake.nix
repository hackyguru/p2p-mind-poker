{
  description = "p2p-poker UI — QML frontend for the poker core module";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    # The core module from this repo. For local core changes, build with:
    #   nix build --override-input poker path:../poker-core '.#lgx-portable'
    poker.url = "github:hackyguru/p2p-mind-poker?dir=poker-core";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
