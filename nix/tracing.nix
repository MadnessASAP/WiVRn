# devenv module providing the Perfetto SDK for the server-tracing presets.
#
# Usage: nix develop --no-pure-eval .#tracing
#        cmake --preset server-tracing && cmake --build build-server-tracing
#
# Only exports PERFETTO_SDK_DIR: the server itself is built with the host
# toolchain and libraries (no Nix compiler in this shell).
{
  pkgs,
  lib,
  perfetto-sdk,
  ...
}:
{
  name = "wivrn-tracing";

  stdenv = pkgs.stdenvNoCC;
  # devenv adds a Nix pkg-config by default, which would hide the host libraries
  packages = lib.mkForce [ ];

  env.PERFETTO_SDK_DIR = "${perfetto-sdk}/share/perfetto/sdk";

  enterShell = ''
    echo "WiVRn tracing shell: Perfetto SDK $PERFETTO_SDK_DIR"
  '';
}
