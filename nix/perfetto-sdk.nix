# Perfetto amalgamated SDK (sdk/perfetto.{h,cc}), for WIVRN_USE_PERFETTO builds.
# Installed where server/CMakeLists.txt looks for it.
{
  lib,
  stdenvNoCC,
  fetchurl,
}:
let
  # Keep in sync with PERFETTO_TAG in .github/workflows/Build.yml
  version = "51.0";
  file =
    name: hash:
    fetchurl {
      url = "https://raw.githubusercontent.com/google/perfetto/v${version}/sdk/${name}";
      inherit hash;
    };
in
stdenvNoCC.mkDerivation {
  pname = "perfetto-sdk";
  inherit version;

  dontUnpack = true;
  installPhase = ''
    install -Dm644 ${file "perfetto.h" "sha256-+FpTfi2rug8CKkzi8qxvDPCEnRknhvYSackaFGz+MWU="} $out/share/perfetto/sdk/perfetto.h
    install -Dm644 ${file "perfetto.cc" "sha256-Tzw4FYp7hKJQZqxL2eEK3/kO3HkTdHUDbNesOOBEWoc="} $out/share/perfetto/sdk/perfetto.cc
  '';

  meta = {
    description = "Perfetto tracing SDK (amalgamated sources)";
    homepage = "https://perfetto.dev/docs/instrumentation/tracing-sdk";
    license = lib.licenses.asl20;
    platforms = lib.platforms.all;
  };
}
