#!/bin/bash
#
# ==============================================================================
# espVT Build Script - ESP-IDF builder for the ESP32-S3 ISOBUS Virtual Terminal
# ==============================================================================
#
# USAGE:
#   ./make_espVT.sh [command] [variant] [options]
#
# COMMANDS:
#   all                       Build all variants (default)
#   <variant>                 Build one variant
#   flash <variant> [PORT]    Flash and monitor a variant (builds it if needed)
#   menuconfig <variant>      Open menuconfig for a variant
#   clean [variant]           Remove build directories (all or one)
#
# OPTIONS:
#   force                     Delete the build directory before building
#
# VARIANTS:
#   default   sdkconfig.defaults                              -> build/
#   rs232     sdkconfig.defaults + sdkconfig.rs232.defaults   -> build-rs232/  (release variant)
#   ntrip     sdkconfig.defaults + sdkconfig.ntrip.defaults   -> build-ntrip/  (UM982 RTK + NTRIP, Wi-Fi)
#
# EXAMPLES:
#   ./make_espVT.sh                     # Build everything
#   ./make_espVT.sh rs232               # Build the RS-232 variant
#   ./make_espVT.sh rs232 force         # Clean rebuild of the RS-232 variant
#   ./make_espVT.sh flash rs232 /dev/ttyACM0
#   ./make_espVT.sh menuconfig ntrip    # Set Wi-Fi/NTRIP credentials (never commit them)
#
# OUTPUT:
#   delivery/espVT-<version>-<VARIANT>.bin   merged image (bootloader + partitions + app), flash at 0x0
#
# ESP-IDF is sourced automatically from IDF_ACTIVATE (default: v6.1) when idf.py is
# not already on PATH.
# ==============================================================================

IDF_ACTIVATE="${IDF_ACTIVATE:-$HOME/.espressif/tools/activate_idf_v6.1.sh}"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ALL_VARIANTS="default rs232 ntrip"

COMMAND="${1:-all}"
ARG2="$2"
ARG3="$3"

if ! command -v idf.py &> /dev/null; then
    echo "----------------------------------------------------------------------------"
    echo "Sourcing ESP-IDF environment: $IDF_ACTIVATE"
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

cd "$PROJECT_DIR" || exit 1

FORCE_REBUILD=false
for arg in "$@"; do
    if [ "$arg" == "force" ]; then
        FORCE_REBUILD=true
        echo "!!! FORCE REBUILD ENABLED !!!"
    fi
done

VERSION="$(git describe --tags --always --dirty 2>/dev/null || echo unknown)"

# Sets BUILD_DIR, SDKCONFIG_FILE, SDKCONFIG_DEFAULTS and DELIVERY_NAME for a variant.
select_variant() {
    case "$1" in
        default)
            BUILD_DIR="build"
            SDKCONFIG_FILE="$PROJECT_DIR/sdkconfig"
            SDKCONFIG_DEFAULTS="sdkconfig.defaults"
            DELIVERY_NAME="espVT-$VERSION.bin"
            ;;
        rs232)
            BUILD_DIR="build-rs232"
            SDKCONFIG_FILE="$PROJECT_DIR/sdkconfig.rs232"
            SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.rs232.defaults"
            DELIVERY_NAME="espVT-$VERSION-RS232.bin"
            ;;
        ntrip)
            BUILD_DIR="build-ntrip"
            SDKCONFIG_FILE="$PROJECT_DIR/sdkconfig.ntrip"
            SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ntrip.defaults"
            DELIVERY_NAME="espVT-$VERSION-NTRIP.bin"
            ;;
        *)
            echo "Error: unknown variant '$1'. Available: $ALL_VARIANTS"
            return 1
            ;;
    esac
}

# All variants are ESP32-S3. Set the target explicitly, so an IDF_TARGET inherited
# from the shell cannot override it.
export IDF_TARGET=esp32s3

idf_variant() {
    idf.py -B "$BUILD_DIR" -D SDKCONFIG="$SDKCONFIG_FILE" -D SDKCONFIG_DEFAULTS="$SDKCONFIG_DEFAULTS" "$@"
}

build_variant() {
    select_variant "$1" || return 1
    echo "============================================================================"
    echo "[$1] building in $BUILD_DIR"
    echo "============================================================================"

    if [ "$FORCE_REBUILD" == true ]; then
        rm -rf "$BUILD_DIR"
    fi
    # The target comes from IDF_TARGET (exported above). No set-target here:
    # it would regenerate the variant's sdkconfig and drop menuconfig settings.

    idf_variant build || return 1
    idf_variant merge-bin || return 1

    mkdir -p delivery || return 1
    cp "$BUILD_DIR/merged-binary.bin" "delivery/$DELIVERY_NAME" || return 1
    echo "[$1] -> delivery/$DELIVERY_NAME"
}

case "$COMMAND" in
    all|force)
        FAILED=""
        for variant in $ALL_VARIANTS; do
            build_variant "$variant" || FAILED="$FAILED $variant"
        done
        echo "----------------------------------------------------------------------------"
        if [ -n "$FAILED" ]; then
            echo " FAILED:$FAILED"
            exit 1
        fi
        echo " DONE. Images in delivery/:"
        ls -1 delivery/
        echo " To flash: ./make_espVT.sh flash <variant> [PORT]"
        echo "----------------------------------------------------------------------------"
        ;;
    flash)
        if [ -z "$ARG2" ]; then
            echo "Usage: ./make_espVT.sh flash <variant> [PORT]"
            exit 1
        fi
        select_variant "$ARG2" || exit 1
        if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
            build_variant "$ARG2" || exit 1
        fi
        PORT_ARGS=()
        if [ -n "$ARG3" ]; then
            PORT_ARGS=(-p "$ARG3")
        fi
        idf_variant "${PORT_ARGS[@]}" flash monitor
        ;;
    menuconfig)
        select_variant "${ARG2:-default}" || exit 1
        if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
            idf_variant set-target esp32s3 || exit 1
        fi
        idf_variant menuconfig
        ;;
    clean)
        for variant in ${ARG2:-$ALL_VARIANTS}; do
            select_variant "$variant" || exit 1
            echo "Removing $BUILD_DIR"
            rm -rf "$BUILD_DIR"
        done
        ;;
    *)
        build_variant "$COMMAND" || exit 1
        ;;
esac
