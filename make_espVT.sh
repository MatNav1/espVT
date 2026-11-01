#!/bin/bash
#
# ==============================================================================
# espVT Build Script - ESP-IDF builder for the ISOBUS Virtual Terminal
# ==============================================================================
#
# USAGE:
#   ./make_espVT.sh [command] [target] [options]
#
# COMMANDS:
#   all                       Build all targets (default)
#   <target>                  Build one target
#   flash <target> [PORT]     Build, then flash and monitor a target
#   menuconfig <target>       Open menuconfig for a target
#   clean [target]            Remove build directories (all or one)
#
# OPTIONS:
#   force                     Delete the build directory before building
#
# TARGETS (chip, sdkconfig layers on top of sdkconfig.defaults + .<chip>):
#   Waveshare7B        esp32s3  Waveshare ESP32-S3-Touch-LCD-7B, no GNSS
#   Waveshare7B_RS232  esp32s3  + NMEA over RS-232 (release target)
#   Waveshare7B_NTRIP  esp32s3  + UM982 RTK with NTRIP over Wi-Fi
#   JC8012P4           esp32p4  Guition JC8012P4A1C_I_W_Y 10.1", no GNSS
#
#   Aliases: default -> Waveshare7B, rs232 -> Waveshare7B_RS232,
#            ntrip -> Waveshare7B_NTRIP, p4 -> JC8012P4
#
# EXAMPLES:
#   ./make_espVT.sh                          # Build everything
#   ./make_espVT.sh Waveshare7B_RS232        # Build one target
#   ./make_espVT.sh JC8012P4 force           # Clean rebuild
#   ./make_espVT.sh flash JC8012P4 /dev/ttyACM5
#   ./make_espVT.sh menuconfig Waveshare7B_NTRIP   # Wi-Fi/NTRIP credentials
#
# OUTPUT:
#   build_<target>/                          build directory incl. its sdkconfig
#   delivery/espVT-<version>-<target>.bin    merged image, flash at 0x0
#
# ESP-IDF is loaded from IDF_ACTIVATE (default: v6.1) when idf.py is not
# already on PATH.
# ==============================================================================

IDF_ACTIVATE="${IDF_ACTIVATE:-$HOME/.espressif/tools/activate_idf_v6.1.sh}"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ALL_TARGETS="Waveshare7B Waveshare7B_RS232 Waveshare7B_NTRIP JC8012P4"

COMMAND="${1:-all}"
ARG2="$2"
ARG3="$3"

# Sets TARGET, CHIP, BUILD_DIR and SDKCONFIG_DEFAULTS for a target name or alias.
select_target() {
    case "$1" in
        default) TARGET="Waveshare7B" ;;
        rs232) TARGET="Waveshare7B_RS232" ;;
        ntrip) TARGET="Waveshare7B_NTRIP" ;;
        p4) TARGET="JC8012P4" ;;
        *) TARGET="$1" ;;
    esac
    case "$TARGET" in
        Waveshare7B)
            CHIP="esp32s3"
            SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.Waveshare7B.esp32s3"
            ;;
        Waveshare7B_RS232)
            CHIP="esp32s3"
            SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.Waveshare7B.esp32s3;sdkconfig.defaults.rs232"
            ;;
        Waveshare7B_NTRIP)
            CHIP="esp32s3"
            SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.Waveshare7B.esp32s3;sdkconfig.defaults.ntrip"
            ;;
        JC8012P4)
            CHIP="esp32p4"
            SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32p4;sdkconfig.defaults.JC8012P4.esp32p4"
            ;;
        *)
            echo "Error: unknown target '$1'. Available: $ALL_TARGETS"
            return 1
            ;;
    esac
    BUILD_DIR="build_$TARGET"
}

cd "$PROJECT_DIR" || exit 1

# clean needs no ESP-IDF environment, so it runs before loading one.
if [ "$COMMAND" == "clean" ]; then
    for target in ${ARG2:-$ALL_TARGETS}; do
        select_target "$target" || exit 1
        echo "Removing $BUILD_DIR"
        rm -rf "$BUILD_DIR"
    done
    exit 0
fi

if ! command -v idf.py &> /dev/null; then
    echo "----------------------------------------------------------------------------"
    echo "Loading ESP-IDF environment: $IDF_ACTIVATE"
    echo "----------------------------------------------------------------------------"
    if [ ! -f "$IDF_ACTIVATE" ]; then
        echo "Error: $IDF_ACTIVATE not found. Set IDF_ACTIVATE to your ESP-IDF activate script."
        exit 1
    fi
    # The activate script refuses to be sourced from another script, so import
    # the environment it prints with -e and define idf.py like it would.
    ACTIVATE_ENV="$(bash "$IDF_ACTIVATE" -e)" || exit 1
    while IFS='=' read -r key value; do
        case "$key" in
            PATH) export PATH="$value:$PATH" ;;
            SYSTEM_PATH|"") ;;
            *) export "$key=$value" ;;
        esac
    done <<< "$ACTIVATE_ENV"
    idf.py() { "$IDF_PYTHON_ENV_PATH/bin/python" "$IDF_PATH/tools/idf.py" "$@"; }
fi

FORCE_REBUILD=false
for arg in "$@"; do
    if [ "$arg" == "force" ]; then
        FORCE_REBUILD=true
        echo "!!! FORCE REBUILD ENABLED !!!"
    fi
done

VERSION="$(git describe --tags --always --dirty 2>/dev/null || echo unknown)"

# IDF_TARGET is set per call, so a target inherited from the shell cannot override it.
idf_target() {
    IDF_TARGET="$CHIP" idf.py -B "$BUILD_DIR" -D SDKCONFIG_DEFAULTS="$SDKCONFIG_DEFAULTS" "$@"
}

# A fresh build directory would otherwise fall back to the esp32 target.
ensure_target() {
    if ! grep -q "^CONFIG_IDF_TARGET=\"$CHIP\"" "$BUILD_DIR/sdkconfig" 2>/dev/null; then
        idf_target set-target "$CHIP" || return 1
    fi
}

build_target() {
    select_target "$1" || return 1
    echo "============================================================================"
    echo "[$TARGET] building $CHIP in $BUILD_DIR"
    echo "============================================================================"

    if [ "$FORCE_REBUILD" == true ]; then
        rm -rf "$BUILD_DIR"
    fi
    # A failed build must not leave an older image under the expected name.
    rm -f "delivery/espVT-$VERSION-$TARGET.bin"
    ensure_target || return 1
    idf_target build || return 1
    idf_target merge-bin || return 1

    mkdir -p delivery || return 1
    cp "$BUILD_DIR/merged-binary.bin" "delivery/espVT-$VERSION-$TARGET.bin" || return 1
    echo "[$TARGET] -> delivery/espVT-$VERSION-$TARGET.bin"
}

case "$COMMAND" in
    all|force)
        FAILED=""
        for target in $ALL_TARGETS; do
            build_target "$target" || FAILED="$FAILED $target"
        done
        echo "----------------------------------------------------------------------------"
        if [ -n "$FAILED" ]; then
            echo " FAILED:$FAILED"
            exit 1
        fi
        echo " DONE. Images in delivery/:"
        ls -1 delivery/
        echo " To flash: ./make_espVT.sh flash <target> [PORT]"
        echo "----------------------------------------------------------------------------"
        ;;
    flash)
        if [ -z "$ARG2" ]; then
            echo "Usage: ./make_espVT.sh flash <target> [PORT]"
            exit 1
        fi
        select_target "$ARG2" || exit 1
        # Always build first, so delivery/ holds the image that gets flashed.
        build_target "$ARG2" || exit 1
        PORT_ARGS=()
        if [ -n "$ARG3" ]; then
            PORT_ARGS=(-p "$ARG3")
        fi
        idf_target "${PORT_ARGS[@]}" flash monitor
        ;;
    menuconfig)
        select_target "${ARG2:-Waveshare7B}" || exit 1
        ensure_target || exit 1
        idf_target menuconfig
        ;;
    *)
        build_target "$COMMAND" || exit 1
        ;;
esac
