#!/usr/bin/env bash

set -eu

ACTION=all
case "${1:-}" in
    ''|-*) ;;                       # no action word, options only
    *)     ACTION=$1; shift ;;
esac

SRC="${WINE_SRC:-$PWD}"
BUILD="${WINE_BUILD:-}"
DIST="${WINE_DIST:-}"
NAME="${TOOL_NAME:-}"
DISPLAY_NAME="${TOOL_DISPLAY_NAME:-}"
STEAM_ROOT="${STEAM_ROOT:-}"
TOOLS_DIR="${TOOLS_DIR:-}"
JOBS="${JOBS:-$( (nproc || sysctl -n hw.ncpu || echo 4) 2>/dev/null )}"
ARCHS="${WINE_ARCHS:-i386,x86_64}"
PROTON_BRANCH="${PROTON_BRANCH:-proton_10.0}"
CONFIGURE_ARGS="${CONFIGURE_ARGS:-}"
WINE_PREFIX_DIR="${WINE_PREFIX_DIR:-}"
LOG_DIR="${WINE_COMPAT_LOG_DIR:-/tmp}"
AS_ROOT="${AS_ROOT:-auto}"
VENDOR=1
RECONFIGURE=0

log()  { printf '%s\n' ":: $*" >&2; }
warn() { printf '%s\n' "!! $*" >&2; }
die()  { printf '%s\n' "!! $*" >&2; exit 1; }

usage() {
    cat >&2 <<USAGE
usage: ${0##*/} [all|build|install|register|doctor|uninstall] [options]

  --src DIR          wine source tree            (default: \$PWD)
  --build DIR        build directory             (default: <src>, an in-tree build)
  --dist DIR         install prefix              (default: <tools-dir>/<name>/dist)
  --name NAME        internal tool name          (default: from <src>/VERSION)
  --display NAME     name shown in Steam         (default: same as --name)
  --steam-root DIR   Steam installation          (default: autodetected)
  --tools-dir DIR    compatibilitytools.d        (default: <steam-root>/compatibilitytools.d)
  --jobs N           parallel make jobs          (default: $JOBS)
  --archs LIST       --enable-archs value        (default: $ARCHS)
  --proton-branch B  branch to vendor lsteamclient from (default: $PROTON_BRANCH)
  --wineprefix DIR   one shared prefix for every game, e.g. ~/.wine
                     (default: unset, each game keeps its own compatdata pfx)
  --log-dir DIR      where per-run logs are written  (default: $LOG_DIR)
  --as-root CMD      how to install into a prefix you cannot write, e.g. doas,
                     sudo, 'doas -u root'; 'none' to fail instead
                     (default: auto, which prefers doas over sudo)
  --no-vendor        skip the lsteamclient vendoring step
  --reconfigure      re-run configure even if the build looks usable
  --configure-args S extra arguments for configure

actions:
  build      vendor lsteamclient, configure, make
  install    make install into the dist directory
  register   write compatibilitytool.vdf, toolmanifest.vdf and the launcher
  all        build + install + register        (default)
  doctor     check an existing installation and the Steam-side plumbing
  uninstall  remove the tool directory

the install directory is passed to make install, so an existing tree keeps
whatever prefix you configured it with by hand and still installs here; only
a prefix you cannot write (--dist /usr/local) escalates, see --as-root.

runtime overrides honored by the launcher:
  STEAM_COMPAT_TOOL_WINEPREFIX   prefix for this launch only
  WINE_COMPAT_LOG_DIR            log directory for this launch only
  WINEDEBUG                      passed through untouched when set
USAGE
    exit 2
}

abspath_str() {
    case "$1" in
        /*) printf '%s\n' "${1%/}" ;;
        *)  printf '%s\n' "${PWD%/}/${1%/}" ;;
    esac
}

while [ $# -gt 0 ]; do
    case "$1" in
        --src)            SRC=$(abspath_str "$2"); shift 2 ;;
        --build)          BUILD=$(abspath_str "$2"); shift 2 ;;
        --dist)           DIST=$(abspath_str "$2"); shift 2 ;;
        --name)           NAME=$2; shift 2 ;;
        --display)        DISPLAY_NAME=$2; shift 2 ;;
        --steam-root)     STEAM_ROOT=$(abspath_str "$2"); shift 2 ;;
        --tools-dir)      TOOLS_DIR=$(abspath_str "$2"); shift 2 ;;
        --jobs|-j)        JOBS=$2; shift 2 ;;
        --archs)          ARCHS=$2; shift 2 ;;
        --proton-branch)  PROTON_BRANCH=$2; shift 2 ;;
        --wineprefix)     WINE_PREFIX_DIR=$(abspath_str "$2"); shift 2 ;;
        --log-dir)        LOG_DIR=$(abspath_str "$2"); shift 2 ;;
        --as-root)        AS_ROOT=$2; shift 2 ;;
        --configure-args) CONFIGURE_ARGS=$2; shift 2 ;;
        --no-vendor)      VENDOR=0; shift ;;
        --reconfigure)    RECONFIGURE=1; shift ;;
        -h|--help)        usage ;;
        *)                die "unknown option: $1 (try --help)" ;;
    esac
done

abspath() { ( cd -- "$(dirname -- "$1")" && printf '%s/%s\n' "$(pwd)" "$(basename -- "$1")" ); }

find_steam_root() {
    [ -n "$STEAM_ROOT" ] && { printf '%s\n' "$STEAM_ROOT"; return; }
    for d in "$HOME/.steam/root" "$HOME/.steam/steam" \
             "${XDG_DATA_HOME:-$HOME/.local/share}/Steam" "$HOME/.local/share/Steam"; do
        [ -d "$d/steamapps" ] && { ( cd -- "$d" && pwd -P ); return; }
    done
    die "cannot find a Steam installation; pass --steam-root"
}

if [ "$ACTION" = "build" ]; then
    STEAM_ROOT=$(find_steam_root 2>/dev/null) || STEAM_ROOT=""
else
    STEAM_ROOT=$(find_steam_root)
fi
if [ -n "$STEAM_ROOT" ] && [ -z "$TOOLS_DIR" ]; then
    TOOLS_DIR="$STEAM_ROOT/compatibilitytools.d"
fi

if [ -z "$NAME" ]; then
    if [ -r "$SRC/VERSION" ]; then
        NAME="wine-$(sed -e 's/^.*version[[:space:]]*//' -e 's/[^A-Za-z0-9._-]/-/g' "$SRC/VERSION")"
    else
        NAME="wine-custom"
    fi
fi
: "${DISPLAY_NAME:=$NAME}"

if [ -n "$TOOLS_DIR" ]; then
    TOOLDIR="$TOOLS_DIR/$NAME"
else
    TOOLDIR=""
fi

if [ -z "$DIST" ]; then
    if [ -n "$TOOLDIR" ]; then
        DIST="$TOOLDIR/dist"
    else
        DIST="$PWD/$NAME/dist"
        warn "no Steam installation found, using prefix $DIST (pass --steam-root or --dist)"
    fi
fi
DIST=$(abspath_str "$DIST")
: "${BUILD:=$SRC}"

makefile_var() {
    sed -n "s/^$2[[:space:]]*=[[:space:]]*\(.*[^[:space:]]\)[[:space:]]*\$/\1/p" \
        "$1/Makefile" 2>/dev/null | head -n 1
}

configured_prefix() { makefile_var "$1" prefix; }

prefix_overridable() {
    local v val
    for v in exec_prefix bindir libdir datarootdir; do
        val=$(makefile_var "$1" "$v")
        case "$val" in
            *'${prefix}'*|*'${exec_prefix}'*|*'${datarootdir}'*) ;;
            *) return 1 ;;
        esac
    done
    return 0
}

stale_reason() {
    local d
    if [ ! -f "$BUILD/Makefile" ]; then
        printf '%s\n' "no Makefile in $BUILD"
        return 0
    fi
    for d in dlls/lsteamclient programs/steamstub dlls/steamstub; do
        [ -f "$SRC/$d/Makefile.in" ] || continue
        grep -q "$d/" "$BUILD/Makefile" && continue
        printf '%s\n' "$d is in the tree but absent from the Makefile"
        return 0
    done
    return 1
}

writable_target() {
    local d=$1
    while [ ! -e "$d" ]; do
        case "$d" in
            */?*) d=${d%/*}; [ -n "$d" ] || d=/ ;;
            *)    d=.; break ;;
        esac
    done
    [ -w "$d" ]
}

root_runner() {
    local c
    case "$AS_ROOT" in
        auto)
            for c in doas sudo; do
                command -v "$c" >/dev/null 2>&1 && { printf '%s\n' "$c"; return 0; }
            done
            return 1
            ;;
        none|no|off|"") return 1 ;;
        *)  command -v "${AS_ROOT%% *}" >/dev/null 2>&1 \
                || die "--as-root ${AS_ROOT%% *}: not found"
            printf '%s\n' "$AS_ROOT"
            ;;
    esac
}

do_build() {
    [ -f "$SRC/configure.ac" ] || die "not a wine tree: $SRC"
    SRC=$(cd -- "$SRC" && pwd -P)

    if [ "$VENDOR" = 1 ] && [ ! -d "$SRC/dlls/lsteamclient" ]; then
        [ -x "$SRC/vendor-lsteamclient.sh" ] \
            || die "no vendor-lsteamclient.sh in $SRC and dlls/lsteamclient is absent"
        log "vendoring lsteamclient from Proton $PROTON_BRANCH"
        ( cd "$SRC" && WINE_TREE="$SRC" BRANCH="$PROTON_BRANCH" ./vendor-lsteamclient.sh )
    else
        log "lsteamclient: $( [ -d "$SRC/dlls/lsteamclient" ] && echo present || echo skipped )"
    fi

    command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1 \
        || warn "no x86_64-w64-mingw32-gcc on PATH, PE builds will fail (install mingw-w64-gcc)"

    mkdir -p "$BUILD"
    BUILD=$(cd -- "$BUILD" && pwd -P)

    local reason="" keep_prefix
    if [ "$RECONFIGURE" = 1 ]; then
        reason="--reconfigure given"
    else
        reason=$(stale_reason) || reason=""
    fi

    if [ -n "$reason" ]; then
        [ -x "$SRC/configure" ] \
            || die "no configure script in $SRC; generate it yourself (this script does not run autoreconf)"
        keep_prefix=$(configured_prefix "$BUILD")
        : "${keep_prefix:=$DIST}"
        log "configuring: $reason"
        [ "$keep_prefix" = "$DIST" ] \
            || log "keeping the tree's prefix $keep_prefix (install still puts files in $DIST)"
        read -r -a extra_args <<< "$CONFIGURE_ARGS"
        set -- --prefix="$keep_prefix" --enable-archs="$ARCHS" --disable-tests \
               ${extra_args[@]+"${extra_args[@]}"}
        if [ "$BUILD" = "$SRC" ]; then
            log "configuring in tree (prefix $keep_prefix)"
            ( cd "$SRC" && ./configure "$@" )
        else
            log "configuring into $BUILD (prefix $keep_prefix)"
            ( cd "$BUILD" && "$SRC/configure" "$@" )
        fi
    else
        log "reusing existing build in $BUILD"
    fi

    log "building with -j$JOBS"
    make -C "$BUILD" "-j$JOBS"

    local a arch_list
    IFS=, read -r -a arch_list <<< "$ARCHS"
    for a in ${arch_list[@]+"${arch_list[@]}"}; do
        [ -n "$a" ] || continue
        [ -f "$BUILD/dlls/lsteamclient/$a-windows/lsteamclient.dll" ] \
            || warn "lsteamclient.dll ($a) was not produced, Steam API calls will not work"
    done
}

do_install() {
    [ -f "$BUILD/Makefile" ] || die "nothing built in $BUILD; run the build action first"

    local have runner=""
    have=$(configured_prefix "$BUILD")
    if [ "$have" != "$DIST" ]; then
        prefix_overridable "$BUILD" || die \
            "the tree in $BUILD installs into ${have:-an unknown prefix} and its install directories are not relative to it; re-run the build action with --reconfigure"
        log "tree is configured for ${have:-an unknown prefix}, installing into $DIST instead"
    fi

    if ! writable_target "$DIST"; then
        runner=$(root_runner) || die \
            "$DIST is not writable and no doas or sudo found (pass --as-root CMD, or --dist somewhere you own)"
        log "$DIST is not writable, installing with: $runner"
    fi

    log "installing into $DIST"
    mkdir -p "$DIST" 2>/dev/null || $runner mkdir -p "$DIST"
    $runner make -C "$BUILD" install prefix="$DIST"

    for f in bin/wine bin/wineserver; do
        [ -x "$DIST/$f" ] || die \
            "$DIST/$f missing after install; the tree may hardcode its install paths, try the build action with --reconfigure"
    done
    for a in i386 x86_64; do
        case ",$ARCHS," in *",$a,"*)
            [ -f "$DIST/lib/wine/$a-windows/lsteamclient.dll" ] \
                || warn "lsteamclient.dll missing for $a"
            [ -f "$DIST/lib/wine/$a-windows/steamstub.exe" ] \
                || warn "steamstub.exe missing for $a"
        ;; esac
    done
}

do_register() {
    [ -n "$TOOLDIR" ] || die "no tools directory resolved"
    [ -x "$DIST/bin/wine" ] || die "no wine at $DIST/bin/wine; run the install action first"
    mkdir -p "$TOOLDIR"

    if [ "$(abspath "$DIST")" != "$(abspath "$TOOLDIR/dist")" ]; then
        ln -sfn "$(abspath "$DIST")" "$TOOLDIR/dist"
    fi

    log "writing $TOOLDIR/compatibilitytool.vdf"
    cat > "$TOOLDIR/compatibilitytool.vdf" <<VDF
"compatibilitytools"
{
  "compat_tools"
  {
    "$NAME"
    {
      "install_path" "."
      "display_name" "$DISPLAY_NAME"
      "from_oslist"  "windows"
      "to_oslist"    "linux"
    }
  }
}
VDF

    log "writing $TOOLDIR/toolmanifest.vdf"
    cat > "$TOOLDIR/toolmanifest.vdf" <<'VDF'
"manifest"
{
  "version" "2"
  "commandline" "/proton %verb%"
}
VDF

    printf '%s\n' "$NAME" > "$TOOLDIR/version"
    ( cd "$DIST/bin" && ./wine --version ) >> "$TOOLDIR/version" 2>/dev/null || true

    log "writing $TOOLDIR/proton"
    {
        printf '%s\n' '#!/usr/bin/env bash'
        printf 'DEFAULT_WINEPREFIX=%s\n' "$(printf '%q' "$WINE_PREFIX_DIR")"
        printf 'DEFAULT_LOG_DIR=%s\n'    "$(printf '%q' "$LOG_DIR")"
    } > "$TOOLDIR/proton"
    cat >> "$TOOLDIR/proton" <<'LAUNCHER'
set -eu

HERE=$(cd -- "$(dirname -- "$0")" && pwd -P)
DIST="$HERE/dist"
WINE="$DIST/bin/wine"
WINESERVER="$DIST/bin/wineserver"

verb="${1:-run}"
[ $# -gt 0 ] && shift || true

STEAMDIR="${STEAM_COMPAT_CLIENT_INSTALL_PATH:-$HOME/.steam/root}"

WINEPREFIX="${STEAM_COMPAT_TOOL_WINEPREFIX:-${DEFAULT_WINEPREFIX:-}}"
if [ -n "$WINEPREFIX" ]; then
    SHARED_PREFIX=1
else
    SHARED_PREFIX=0
    : "${STEAM_COMPAT_DATA_PATH:?must be launched by Steam, or register with --wineprefix}"
    WINEPREFIX="$STEAM_COMPAT_DATA_PATH/pfx"
fi

export PATH="$DIST/bin:$PATH"
export LD_LIBRARY_PATH="$DIST/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export WINEPREFIX
[ -n "${WINEDEBUG+x}" ] && export WINEDEBUG

: "${SteamAppId:=${STEAM_COMPAT_APP_ID:-0}}"
: "${SteamGameId:=$SteamAppId}"
export SteamAppId SteamGameId

LOGDIR="${WINE_COMPAT_LOG_DIR:-${DEFAULT_LOG_DIR:-/tmp}}"
mkdir -p "$LOGDIR"
LOGFILE="$LOGDIR/${HERE##*/}-${SteamAppId}-$(date +%Y%m%d-%H%M%S)-$$.log"
printf 'log: %s\n' "$LOGFILE" >&2
case "$verb" in
    getcompatpath|getnativepath) exec 2>>"$LOGFILE" ;;
    *)                           exec >>"$LOGFILE" 2>&1 ;;
esac

log() { printf '%s\n' "[${0##*/}] $*" >&2; }

log "verb=$verb appid=$SteamAppId prefix=$WINEPREFIX wine=$("$WINE" --version 2>/dev/null)"
log "WINEDEBUG=${WINEDEBUG-<unset>} args: $*"

ensure_sdk_links() {
    mkdir -p "$HOME/.steam"
    for bits in 32 64; do
        [ -e "$HOME/.steam/sdk$bits" ] && continue
        if [ -d "$STEAMDIR/linux$bits" ]; then
            ln -sfn "$STEAMDIR/linux$bits" "$HOME/.steam/sdk$bits"
            log "created ~/.steam/sdk$bits -> $STEAMDIR/linux$bits"
        else
            log "WARNING: $STEAMDIR/linux$bits not found, ~/.steam/sdk$bits unresolved"
        fi
    done
}

setup_prefix() {
    mkdir -p "$WINEPREFIX"
    log "initializing prefix $WINEPREFIX"
    "$WINE" wineboot -u
    "$WINESERVER" -w

    pfx_steam="$WINEPREFIX/drive_c/Program Files (x86)/Steam"
    mkdir -p "$pfx_steam"

    client32='C:\\windows\\system32\\lsteamclient.dll'
    client64='C:\\windows\\system32\\lsteamclient.dll'
    for f in steamclient.dll steamclient64.dll Steam.dll \
             GameOverlayRenderer.dll GameOverlayRenderer64.dll; do
        if [ -f "$STEAMDIR/legacycompat/$f" ]; then
            cp -f "$STEAMDIR/legacycompat/$f" "$pfx_steam/$f"
        fi
    done
    [ -f "$pfx_steam/steamclient.dll" ]   && client32='C:\\Program Files (x86)\\Steam\\steamclient.dll'
    [ -f "$pfx_steam/steamclient64.dll" ] && client64='C:\\Program Files (x86)\\Steam\\steamclient64.dll'
    log "SteamClientDll set to $client32"

    reg=$(mktemp); trap 'rm -f "$reg"' RETURN
    cat > "$reg" <<REG
Windows Registry Editor Version 5.00

[HKEY_CURRENT_USER\\Software\\Valve\\Steam]
"SteamPath"="C:\\\\Program Files (x86)\\\\Steam"
"SteamExe"="C:\\\\Program Files (x86)\\\\Steam\\\\steam.exe"
"SourceModInstallPath"="C:\\\\Program Files (x86)\\\\Steam\\\\steamapps\\\\sourcemods"

[HKEY_CURRENT_USER\\Software\\Valve\\Steam\\ActiveProcess]
"SteamClientDll"="$client32"
"SteamClientDll64"="$client64"
"Universe"="Public"

[HKEY_LOCAL_MACHINE\\Software\\Valve\\Steam]
"InstallPath"="C:\\\\Program Files (x86)\\\\Steam"
REG
    "$WINE" regedit /S "$reg"
    "$WINESERVER" -w
}

ensure_sdk_links

mkdir -p "$WINEPREFIX"
stamp="$WINEPREFIX/.compat_tool_version"
want="$(cat "$HERE/version" 2>/dev/null || echo unknown)"
stamped() { [ "$(cat "$stamp" 2>/dev/null || true)" = "$want" ]; }

if ! stamped; then
    if command -v flock >/dev/null 2>&1; then
        exec 9>"$WINEPREFIX/.compat_tool_lock"
        flock 9
        stamped || { setup_prefix; printf '%s\n' "$want" > "$stamp"; }
        flock -u 9
        exec 9>&-
    else
        setup_prefix
        printf '%s\n' "$want" > "$stamp"
    fi
fi

case "$verb" in
    run|waitforexitandrun)
        if [ "$verb" = waitforexitandrun ] && [ "$SHARED_PREFIX" = 0 ]; then
            "$WINESERVER" -w
        fi
        [ $# -gt 0 ] || { log "no command given"; exit 2; }
        exe=$1; shift
        exe_w=$("$WINE" winepath -w "$exe" 2>/dev/null) || exe_w=$exe
        exec "$WINE" steamstub.exe "$exe_w" "$@"
        ;;
    runinprefix)
        exec "$WINE" "$@"
        ;;
    getcompatpath)  exec "$WINE" winepath -w "${1:?path required}" ;;
    getnativepath)  exec "$WINE" winepath -u "${1:?path required}" ;;
    *) log "unknown verb: $verb"; exit 2 ;;
esac
LAUNCHER
    chmod +x "$TOOLDIR/proton"

    log "registered '$NAME' in $TOOLS_DIR"
    log "restart Steam, then pick '$DISPLAY_NAME' under Compatibility"
}

do_doctor() {
    rc=0
    check() { if eval "$2"; then printf '  ok   %s\n' "$1"; else printf '  FAIL %s\n' "$1"; rc=1; fi; }

    printf 'steam root:  %s\n' "$STEAM_ROOT"
    cfg=$(sed -n "s/^DEFAULT_WINEPREFIX=//p" "$TOOLDIR/proton" 2>/dev/null | sed "s/^'//; s/'\$//" || true)
    printf 'wineprefix:  %s\n' "${cfg:-<per-game compatdata>}"
    cfgl=$(sed -n "s/^DEFAULT_LOG_DIR=//p" "$TOOLDIR/proton" 2>/dev/null | sed "s/^'//; s/'\$//" || true)
    printf 'log dir:     %s\n' "${cfgl:-/tmp}"
    printf 'tool dir:    %s\n' "$TOOLDIR"
    if [ -f "$BUILD/Makefile" ]; then
        bp=$(configured_prefix "$BUILD")
        printf 'build tree:  %s (configured prefix %s, installs into %s)\n' \
            "$BUILD" "${bp:-unknown}" "$DIST"
    fi
    check "tool directory exists"            '[ -d "$TOOLDIR" ]'
    check "compatibilitytool.vdf present"    '[ -f "$TOOLDIR/compatibilitytool.vdf" ]'
    check "toolmanifest.vdf present"         '[ -f "$TOOLDIR/toolmanifest.vdf" ]'
    check "launcher is executable"           '[ -x "$TOOLDIR/proton" ]'
    check "wine binary runs"                 '"$TOOLDIR/dist/bin/wine" --version >/dev/null 2>&1'
    check "lsteamclient.dll (x86_64) built"  '[ -f "$TOOLDIR/dist/lib/wine/x86_64-windows/lsteamclient.dll" ]'
    check "lsteamclient.dll (i386) built"    '[ -f "$TOOLDIR/dist/lib/wine/i386-windows/lsteamclient.dll" ]'
    check "steamstub.exe built"              '[ -f "$TOOLDIR/dist/lib/wine/x86_64-windows/steamstub.exe" ]'
    check "~/.steam/sdk64/steamclient.so"    '[ -e "$HOME/.steam/sdk64/steamclient.so" ]'
    check "~/.steam/sdk32/steamclient.so"    '[ -e "$HOME/.steam/sdk32/steamclient.so" ]'
    check "legacycompat shims in Steam"      '[ -f "$STEAM_ROOT/legacycompat/steamclient64.dll" ]'
    check "/dev/ntsync present"              '[ -e /dev/ntsync ]'
    [ "$rc" = 0 ] && printf '\nall good\n' || printf '\nsee failures above\n'
    return "$rc"
}

do_uninstall() {
    [ -n "$TOOLDIR" ] && [ -d "$TOOLDIR" ] || die "nothing at $TOOLDIR"
    log "removing $TOOLDIR"
    rm -rf -- "$TOOLDIR"
}

case "$ACTION" in
    build)     do_build ;;
    install)   do_install ;;
    register)  do_register ;;
    all)       do_build; do_install; do_register ;;
    doctor)    do_doctor ;;
    uninstall) do_uninstall ;;
    *)         usage ;;
esac
