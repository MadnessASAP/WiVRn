# devenv module for building the Android (headset) client with Gradle.
#
# Usage: nix develop --no-pure-eval .#android
#        ./gradlew assembleDebug
#
# SDK versions must match build.gradle: Gradle cannot install missing SDK
# packages in the read-only Nix store.
{ pkgs, lib, ... }:
{
  name = "wivrn-android";

  android = {
    enable = true;
    platforms.version = [ "34" ]; # compileSdkVersion
    buildTools.version = [ "35.0.0" ]; # Android Gradle plugin 8.13 default
    ndk.version = [ "29.0.14206865" ]; # ndkVersion
    cmake.version = [ "3.31.5" ]; # externalNativeBuild.cmake.version

    # Only the build toolchain, no emulator or system images
    emulator.enable = false;
    systemImages.enable = false;
    googleAPIs.enable = false;
    googleTVAddOns.enable = false;
    extras = [ ];
  };

  # Same JDK as the GitHub runners
  languages.java.jdk.package = pkgs.jdk17;

  # Host tools used by the client build (see CMakeLists.txt and build.gradle)
  packages = [
    pkgs.git
    pkgs.pkg-config
    pkgs.gnumake # OpenSSL build
    pkgs.perl # OpenSSL Configure
    pkgs.gettext
    pkgs.glslang
    pkgs.spirv-tools
    pkgs.librsvg # rsvg-convert
    pkgs.ktx-tools # ktx
    pkgs.util-linux # hexdump
  ];

  # Let Gradle find the NDK from ndkVersion, ndk.dir is deprecated
  enterShell = lib.mkAfter ''
    sed -i '/^ndk.dir=/d' local.properties
    echo "WiVRn Android shell: NDK $ANDROID_NDK_ROOT"
  '';
}
