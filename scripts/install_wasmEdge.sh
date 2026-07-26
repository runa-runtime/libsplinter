#!/bin/bash
# scripts/install_wasmEdge.sh — build and install WasmEdge from source
#
# Splinter's `--with-wasm` build links against libwasmedge. This script clones
# WasmEdge into /usr/local/src (so it can be git-pulled and rebuilt later, the
# same way bigbang.sh treats llama.cpp), compiles it, and installs it to
# /usr/local with sudo.
#
# Usage:
#   scripts/install_wasmEdge.sh [--ref <tag|branch>] [--prefix <dir>] [--yes]
#
# Environment overrides:
#   WASMEDGE_REF     git tag or branch to build   (default: latest release tag)
#   PREFIX           install prefix               (default: /usr/local)
#   SRC_ROOT         source checkout root         (default: /usr/local/src)

set -euo pipefail

BOLD='\033[1m'
CYAN='\033[0;36m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
DIM='\033[2m'
RESET='\033[0m'

LOG="$HOME/wasmedge_install.log"
SRC_ROOT="${SRC_ROOT:-/usr/local/src}"
PREFIX="${PREFIX:-/usr/local}"
WASMEDGE_SRC="$SRC_ROOT/WasmEdge"
WASMEDGE_REPO="https://github.com/WasmEdge/WasmEdge.git"
WASMEDGE_REF="${WASMEDGE_REF:-}"
ASSUME_YES=0
NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)

usage() {
    sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --ref)    WASMEDGE_REF="${2:?--ref requires a tag or branch}"; shift 2 ;;
        --prefix) PREFIX="${2:?--prefix requires a directory}";        shift 2 ;;
        --yes|-y) ASSUME_YES=1; shift ;;
        --help|-h) usage ;;
        *) printf "Unknown option: %s (try --help)\n" "$1" >&2; exit 2 ;;
    esac
done

touch "$LOG"
exec > >(tee -a "$LOG") 2>&1

ts()      { date '+%Y-%m-%d %H:%M:%S'; }
log()     { printf "${DIM}[%s]${RESET} %s\n" "$(ts)" "$*"; }
section() {
    printf "\n${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
    printf "  %s\n" "$*"
    printf "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RESET}\n"
    log "SECTION: $*"
}
ok()   { printf "${GREEN}  ✓  %s${RESET}\n" "$*";   log "OK: $*";   }
warn() { printf "${YELLOW}  ⚠  %s${RESET}\n" "$*";  log "WARN: $*"; }
die()  {
    printf "${RED}${BOLD}  ✗  FATAL: %s${RESET}\n" "$*"
    log "FATAL: $*"
    printf "\nInstallation log: ${BOLD}%s${RESET}\n" "$LOG"
    exit 1
}

# Reads a keystroke directly from the terminal even when stdin is a pipe.
confirm_sudo() {
    local msg="${1:-The next step requires sudo access.}"
    [[ "$ASSUME_YES" -eq 1 ]] && { log "Auto-confirmed (--yes): $msg"; return; }
    printf "\n${YELLOW}${BOLD}  ⚠  %s${RESET}\n" "$msg"
    printf "     Press ${BOLD}ENTER${RESET} to continue or ${BOLD}Ctrl-C${RESET} to cancel … "
    read -r < /dev/tty
    printf "\n"
}

printf "\n${BOLD}${CYAN}  WasmEdge source installer for Splinter${RESET}\n"
printf "  Log: ${DIM}%s${RESET}\n" "$LOG"
log "install_wasmEdge.sh started"

section "Toolchain Check"

for _tool in git cmake; do
    command -v "$_tool" &>/dev/null || die "'$_tool' not found. Run ./configure --install-deb-deps first."
done
ok "git and cmake present."

# WasmEdge's AOT compiler needs LLVM + lld development headers. On Debian and
# Ubuntu these come from llvm-dev, lld, liblld-dev and libzstd-dev — exactly the
# set ./configure --install-deb-deps installs.
if ! command -v llvm-config &>/dev/null && ! ls /usr/lib/llvm-*/bin/llvm-config &>/dev/null; then
    warn "llvm-config not found — the AOT compiler may fail to configure."
    warn "On Debian/Ubuntu: ./configure --install-deb-deps"
fi

if command -v ninja &>/dev/null; then
    GENERATOR=(-GNinja)
    ok "Using Ninja generator."
else
    GENERATOR=()
    warn "ninja not found — falling back to Make (slower)."
fi

if ! sudo -v 2>/dev/null; then
    die "sudo is required to install into $PREFIX."
fi
ok "sudo access confirmed."

section "Fetching WasmEdge Source"

# Sources are kept under /usr/local/src so they can be updated (git pull) and
# rebuilt later. Make the directory writable by the current user so subsequent
# updates don't require sudo for the clone/build steps.
if [[ ! -d "$SRC_ROOT" ]]; then
    log "Preparing source directory $SRC_ROOT…"
    sudo mkdir -p "$SRC_ROOT"
fi
if [[ ! -w "$SRC_ROOT" ]]; then
    sudo chown "$(id -u):$(id -g)" "$SRC_ROOT"
fi
ok "Source directory ready: $SRC_ROOT"

if [[ -d "$WASMEDGE_SRC/.git" ]]; then
    log "Existing checkout found — updating $WASMEDGE_SRC…"
    git -C "$WASMEDGE_SRC" remote set-url origin "$WASMEDGE_REPO"
    git -C "$WASMEDGE_SRC" fetch --tags --force origin
    ok "Fetched latest refs."
else
    [[ -e "$WASMEDGE_SRC" ]] && { warn "Removing non-git path $WASMEDGE_SRC"; rm -rf "$WASMEDGE_SRC"; }
    log "Cloning WasmEdge into $WASMEDGE_SRC…"
    git clone "$WASMEDGE_REPO" "$WASMEDGE_SRC"
    ok "Clone complete."
fi

# Default to the newest release tag rather than an in-flight master.
if [[ -z "$WASMEDGE_REF" ]]; then
    WASMEDGE_REF=$(git -C "$WASMEDGE_SRC" tag --list --sort=-v:refname \
        | grep -E '^[0-9]+\.[0-9]+\.[0-9]+$' | head -1 || true)
    [[ -n "$WASMEDGE_REF" ]] || die "Could not determine a release tag; pass --ref explicitly."
    log "Latest release tag: $WASMEDGE_REF"
fi

log "Checking out $WASMEDGE_REF…"
git -C "$WASMEDGE_SRC" checkout --force "$WASMEDGE_REF"
# Fast-forward when the ref is a branch; a detached tag has no upstream.
git -C "$WASMEDGE_SRC" pull --ff-only 2>/dev/null || true
git -C "$WASMEDGE_SRC" submodule update --init --recursive
ok "Source at $WASMEDGE_REF ($(git -C "$WASMEDGE_SRC" rev-parse --short HEAD))."

section "Building WasmEdge"

log "Configuring (prefix $PREFIX)…"
cmake -S "$WASMEDGE_SRC" -B "$WASMEDGE_SRC/build" "${GENERATOR[@]}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DWASMEDGE_BUILD_TESTS=OFF \
    -DWASMEDGE_BUILD_SHARED_LIB=ON

log "Building with $NPROC jobs (this takes several minutes)…"
cmake --build "$WASMEDGE_SRC/build" --config Release -j"$NPROC"
ok "WasmEdge built."

section "Installing WasmEdge"
confirm_sudo "About to install WasmEdge to $PREFIX and run ldconfig."

log "sudo cmake --install…"
sudo cmake --install "$WASMEDGE_SRC/build" --prefix "$PREFIX"

log "ldconfig…"
sudo ldconfig
ok "WasmEdge installed to $PREFIX."

section "Verifying"

hash -r
if command -v wasmedge &>/dev/null; then
    ok "wasmedge: $(wasmedge --version 2>/dev/null | head -1)"
else
    warn "wasmedge not on PATH — check that $PREFIX/bin is in your PATH."
fi

if pkg-config --exists wasmedge 2>/dev/null; then
    ok "pkg-config sees wasmedge $(pkg-config --modversion wasmedge)"
elif [[ -f "$PREFIX/include/wasmedge/wasmedge.h" ]]; then
    ok "Headers at $PREFIX/include/wasmedge/wasmedge.h (CMake will find these)."
else
    warn "wasmedge.h not found under $PREFIX/include — Splinter's WITH_WASM may fail to configure."
fi

section "All Done"

printf "\n${BOLD}  Installed:${RESET}\n"
printf "    wasmedge      →  %s\n" "$(command -v wasmedge 2>/dev/null || echo "$PREFIX/bin/wasmedge")"
printf "    libwasmedge   →  %s/lib\n" "$PREFIX"
printf "    headers       →  %s/include/wasmedge\n" "$PREFIX"
printf "    source        →  %s (%s)\n" "$WASMEDGE_SRC" "$WASMEDGE_REF"
printf "    log           →  %s\n\n" "$LOG"
printf "  Build Splinter against it with:\n"
printf "    ${BOLD}./configure --with-wasm${RESET}\n"
printf "    ${BOLD}make && sudo -E make install${RESET}\n\n"

log "install_wasmEdge.sh completed successfully."
