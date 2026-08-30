{
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { nixpkgs, ... }:
    let
      supportedSystems = [
        "aarch64-darwin"
        "aarch64-linux"
        "x86_64-linux"
      ];
      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
    in
    {
      devShells = forAllSystems (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default = pkgs.mkShell {
            packages = with pkgs; [
              llvmPackages.clang
              llvmPackages.clang-tools
              cmake
              cmake-format
              ninja
              ccache
              emscripten
              conan
              just
              watchexec
              browser-sync
              nixfmt
              statix
              deadnix
              prettier
              htmlhint
              stylelint
              actionlint
              cmake-language-server
              nixd
              vscode-langservers-extracted
              python3
            ];

            shellHook = ''
              export PATH="${pkgs.llvmPackages.clang-tools}/bin:${pkgs.llvmPackages.clang}/bin:$PATH"
              export CC="${pkgs.llvmPackages.clang}/bin/clang"
              export CXX="${pkgs.llvmPackages.clang}/bin/clang++"
              export CMAKE_GENERATOR=Ninja
              export EMSCRIPTEN_ROOT="${pkgs.emscripten}/share/emscripten"
              export EM_CACHE="''${XDG_CACHE_HOME:-$HOME/.cache}/emscripten"
              export CCACHE_DIR="''${XDG_CACHE_HOME:-$HOME/.cache}/ccache/portfolio"
              export CCACHE_COMPRESS=true
              export CCACHE_MAXSIZE=2G

              mkdir -p "$EM_CACHE" "$CCACHE_DIR"
            '';
          };
        }
      );

      formatter = forAllSystems (system: nixpkgs.legacyPackages.${system}.nixfmt-tree);
    };
}
