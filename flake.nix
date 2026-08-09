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
        cudaSupported = system == "x86_64-linux";
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
          ] ++ pkgs.lib.optionals cudaSupported [
            pkgs.cudaPackages.cudatoolkit
          ];

          shellHook = ''
            export PATH="$PWD/build/bin:$PATH"
          '' + pkgs.lib.optionalString cudaSupported ''
            export CUDA_PATH="${pkgs.cudaPackages.cudatoolkit}"
            export CUDAToolkit_ROOT="${pkgs.cudaPackages.cudatoolkit}"
            # libcuda.so.1 comes from the host NVIDIA driver:
            # /run/opengl-driver/lib on NixOS, /usr/lib on Arch and friends.
            for _driver_dir in /run/opengl-driver/lib /usr/lib; do
              if [ -e "$_driver_dir/libcuda.so.1" ]; then
                export LD_LIBRARY_PATH="$_driver_dir''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
                break
              fi
            done
          '';

          buildInputs = [
            pkgs.zlib
            pkgs.zstd
          ];
        };
      });
}
