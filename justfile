set shell := ["bash", "-euo", "pipefail", "-c"]

default:
    @just --list

setup: hooks configure-native configure-web

hooks:
    hk install

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
    oxfmt --write CMakePresets.json '.github/**/*.yml' 'web/**/*.{html,css}' .oxfmtrc.json .stylelintrc.json

fmt-check:
    just --fmt --check
    clang-format --dry-run --Werror $(find src -type f \( -name '*.cpp' -o -name '*.hpp' \))
    cmake-format --check CMakeLists.txt
    nixfmt --check flake.nix
    oxfmt --check CMakePresets.json '.github/**/*.yml' 'web/**/*.{html,css}' .oxfmtrc.json .stylelintrc.json

lint: configure-native configure-web
    #!/usr/bin/env bash
    set -euo pipefail

    clang-tidy --quiet -p build/native-debug src/main.cpp

    em++ -x c++ -E /dev/null -o /dev/null
    sysroot="$(em-config CACHE)/sysroot"
    resource_dir="$(em++ -print-resource-dir)"
    clang_tidy_dir="$(dirname "$(command -v clang-tidy)")"
    clang_tidy="$clang_tidy_dir/clang-tidy-unwrapped"
    if [[ ! -x "$clang_tidy" ]]; then
      clang_tidy=clang-tidy
    fi
    env -u NIX_CFLAGS_COMPILE "$clang_tidy" --quiet src/renderer.cpp -- \
      -target wasm32-unknown-emscripten \
      -std=c++23 \
      -nostdinc \
      -nostdinc++ \
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
    hk validate

flake-check:
    nix flake check

web-budget: build-web
    #!/usr/bin/env bash
    set -euo pipefail
    shopt -s nullglob

    site="build/web-release/site"
    check_asset() {
      local label="$1"
      local budget="$2"
      shift 2
      local files=("$@")
      if (( ${#files[@]} != 1 )); then
        printf 'Expected one %s asset, found %d\n' "$label" "${#files[@]}" >&2
        return 1
      fi

      local bytes
      bytes="$(wc -c < "${files[0]}")"
      if (( bytes > budget )); then
        printf '%s exceeds its budget: %d > %d bytes\n' "$label" "$bytes" "$budget" >&2
        return 1
      fi
      printf '%-10s %6d / %6d bytes\n' "$label" "$bytes" "$budget"
    }

    check_asset JavaScript $((16 * 1024)) "$site"/portfolio.*.js
    check_asset WebAssembly $((40 * 1024)) "$site"/portfolio.*.wasm
    check_asset CSS $((3 * 1024)) "$site"/styles.*.css
    check_asset HTML $((4 * 1024)) "$site"/index.html

check: flake-check fmt-check lint build-native web-budget

clean:
    cmake -E remove_directory build
