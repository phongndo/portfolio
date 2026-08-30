set shell := ["bash", "-euo", "pipefail", "-c"]

default:
  @just --list

setup: configure-native configure-web

configure-native:
  cmake --preset native-debug

configure-web:
  cmake --preset web-release

build-native: configure-native
  cmake --build --preset native-debug

build-web: configure-web
  cmake --build --preset web-release

serve host="127.0.0.1" port="8080": build-web
  python3 -m http.server "{{port}}" --bind "{{host}}" --directory build/web-release/site

fmt:
  clang-format -i $(find src -type f \( -name '*.cpp' -o -name '*.hpp' \))
  cmake-format -i CMakeLists.txt
  nixfmt flake.nix
  prettier --write CMakePresets.json '.github/**/*.yml' 'web/**/*.{html,css}' .prettierrc.json .stylelintrc.json

fmt-check:
  clang-format --dry-run --Werror $(find src -type f \( -name '*.cpp' -o -name '*.hpp' \))
  cmake-format --check CMakeLists.txt
  nixfmt --check flake.nix
  prettier --check CMakePresets.json '.github/**/*.yml' 'web/**/*.{html,css}' .prettierrc.json .stylelintrc.json

lint: configure-native
  clang-tidy -p build/native-debug src/main.cpp
  cmake-lint CMakeLists.txt
  statix check flake.nix
  deadnix --fail flake.nix
  htmlhint 'web/**/*.html'
  stylelint 'web/**/*.css'
  actionlint

flake-check:
  nix flake check

check: flake-check fmt-check lint build-native build-web

clean:
  cmake -E remove_directory build
