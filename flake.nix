{
  description = "sammine-lang compiler";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs {
          inherit system;
          config.allowUnfree = true;
        };
        llvmPackages = pkgs.llvmPackages_20;
      in
      {
        devShells.default = pkgs.mkShell.override { stdenv = llvmPackages.stdenv; } {
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.ccache
            pkgs.just
            pkgs.pkg-config
            (pkgs.python3.withPackages (ps: [ ps.lit ]))
            pkgs.git
            pkgs.mold
          ];

          shellHook = ''
            export PATH="$PWD/build/bin:$PATH"

            # Detect CUDA toolkit: check PATH first, then common system locations.
            if ! command -v nvcc &>/dev/null; then
              for _cuda_dir in /opt/cuda /usr/local/cuda; do
                if [ -x "$_cuda_dir/bin/nvcc" ]; then
                  export PATH="$_cuda_dir/bin:$PATH"
                  break
                fi
              done
            fi
            if command -v nvcc &>/dev/null; then
              export NVCC_PATH="$(command -v nvcc)"
              export CUDA_PATH="$(dirname "$(dirname "$NVCC_PATH")")"
            fi
            if [ -d /usr/lib ] && [ -e /usr/lib/libcuda.so.1 ]; then
              export LD_LIBRARY_PATH="/usr/lib:''${LD_LIBRARY_PATH:-}"
            fi
          '';

          buildInputs = [
            pkgs.zlib
            pkgs.zstd
          ];
        };
      });
}
