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
    python3 -m http.server "{{ port }}" --bind "{{ host }}" --directory build/web-release/site

watch host="127.0.0.1" port="8080":
    #!/usr/bin/env bash
    set -euo pipefail

    just build-web

    watcher_pid=""
    cleanup() {
      if [[ -n "$watcher_pid" ]]; then
        kill "$watcher_pid" 2>/dev/null || true
        wait "$watcher_pid" 2>/dev/null || true
      fi
    }
    trap cleanup EXIT INT TERM

    watchexec \
      --watch src \
      --watch web \
      --watch CMakeLists.txt \
      --watch CMakePresets.json \
      --exts cpp,hpp,html,css,txt,json \
      --debounce 100ms \
      -- just build-web &
    watcher_pid=$!

    browser-sync start \
      --server build/web-release/site \
      --files 'build/web-release/site/**/*' \
      --host "{{ host }}" \
      --port "{{ port }}" \
      --no-ui \
      --no-notify

fmt:
    just --fmt
    clang-format -i $(find src -type f \( -name '*.cpp' -o -name '*.hpp' \))
    cmake-format -i CMakeLists.txt
    nixfmt flake.nix
    prettier --write CMakePresets.json '.github/**/*.yml' 'web/**/*.{html,css}' .prettierrc.json .stylelintrc.json

fmt-check:
    just --fmt --check
    clang-format --dry-run --Werror $(find src -type f \( -name '*.cpp' -o -name '*.hpp' \))
    cmake-format --check CMakeLists.txt
    nixfmt --check flake.nix
    prettier --check CMakePresets.json '.github/**/*.yml' 'web/**/*.{html,css}' .prettierrc.json .stylelintrc.json

lint: configure-native configure-web
    #!/usr/bin/env bash
    set -euo pipefail

    clang-tidy --quiet -p build/native-debug src/main.cpp

    sysroot="$(em-config CACHE)/sysroot"
    resource_dir="$(em++ -print-resource-dir)"
    clang-tidy --quiet src/renderer.cpp -- \
      -target wasm32-unknown-emscripten \
      -std=c++23 \
      -isysroot "$sysroot" \
      -resource-dir "$resource_dir" \
      -isystem "$sysroot/include/wasm32-emscripten/c++/v1" \
      -isystem "$sysroot/include/c++/v1" \
      -isystem "$resource_dir/include" \
      -isystem "$sysroot/include/wasm32-emscripten" \
      -isystem "$sysroot/include" \
      -iwithsysroot/include/fakesdl \
      -iwithsysroot/include/compat

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
