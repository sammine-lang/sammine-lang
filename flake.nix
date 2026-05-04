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
            pkgs.python3
            pkgs.git
          ];

          buildInputs = [
            pkgs.zlib
            pkgs.zstd
          ] ++ pkgs.lib.optionals (pkgs.stdenv.hostPlatform.isLinux) [
            pkgs.cudaPackages.cuda_cudart
            pkgs.cudaPackages.cuda_nvcc
          ];
        };
      });
}
