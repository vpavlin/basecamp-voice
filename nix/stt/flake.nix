{
  description = "Parakeet speech-to-text (whisper.cpp) as one static library for basecamp_voice_core";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    # Parakeet is on whisper.cpp master only (after v1.9.4). Pinned.
    whisper-src = {
      url = "github:ggml-org/whisper.cpp/60c0be6ac8fa71b1a2ae2dd938a31a34a508e774";
      flake = false;
    };
  };

  outputs = { nixpkgs, whisper-src, ... }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" "aarch64-darwin" ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in {
      packages = forAll (pkgs: {
        # lib/libvoicestt.a (parakeet + ggml, PIC) and the headers, flat.
        default = pkgs.stdenv.mkDerivation {
          pname = "voicestt";
          version = "0-60c0be6";
          src = whisper-src;
          nativeBuildInputs = [ pkgs.cmake ];
          cmakeFlags = [
            "-DBUILD_SHARED_LIBS=OFF"
            "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
            # Runs on any user's machine: no -march=native.
            "-DGGML_NATIVE=OFF"
            # ggml's own thread pool; no libgomp to ship.
            "-DGGML_OPENMP=OFF"
            "-DWHISPER_BUILD_EXAMPLES=OFF"
            "-DWHISPER_BUILD_TESTS=OFF"
            "-DWHISPER_BUILD_SERVER=OFF"
            "-DWHISPER_SDL2=OFF"
          ];
          buildPhase = ''
            runHook preBuild
            cmake --build . --target parakeet -j $NIX_BUILD_CORES
            runHook postBuild
          '';
          installPhase = ''
            runHook preInstall
            mkdir -p $out/lib $out/include
            libs=$(find . -name 'libparakeet.a' -o -name 'libggml.a' -o -name 'libggml-base.a' -o -name 'libggml-cpu.a')
            echo "merging: $libs"
            {
              echo "CREATE $out/lib/libvoicestt.a"
              for l in $libs; do echo "ADDLIB $l"; done
              echo "SAVE"
              echo "END"
            } | ${if pkgs.stdenv.isDarwin then "false" else "ar -M"}
            ranlib $out/lib/libvoicestt.a
            cp $src/include/parakeet.h $out/include/
            cp $src/ggml/include/*.h $out/include/
            runHook postInstall
          '';
        };
      });
    };
}
