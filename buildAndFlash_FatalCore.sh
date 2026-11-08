#!/usr/bin/env bash
# Build / flash the Flipper Zero ESP32 port for the Fatal Core board.
#
#   ./buildAndFlash_FatalCore.sh                 # build + flash (auto port)
#   ./buildAndFlash_FatalCore.sh --build-only    # build only
#   ./buildAndFlash_FatalCore.sh -p /dev/ttyACM0 # explicit port
#   ./buildAndFlash_FatalCore.sh -m              # open monitor after flash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PORT="${ESPPORT:-}"
RUN_MONITOR=0
BUILD_ONLY=0
EXPORT_SCRIPT="${ESP_IDF_EXPORT_SCRIPT:-${HOME}/esp/esp-idf/export.sh}"

BOARD="fatalcore"
BUILD_DIR="build_fatalcore"
IDF_TARGET="esp32s3"

usage() {
    cat <<EOF
Usage: $(basename "$0") [--port <device>] [--monitor] [--build-only]

Builds the ESP32-S3 Flipper Zero port for the Fatal Core board.

Options:
  -p, --port <device>  Serial device to flash (default: auto-detect)
  -m, --monitor        Open idf.py monitor after flashing
      --build-only     Build the firmware, skip flashing
  -h, --help           Show this help

Environment:
  ESPPORT                  Overrides the auto-detected serial device
  ESP_IDF_EXPORT_SCRIPT    Overrides the ESP-IDF export.sh path
EOF
}

detect_port() {
    local matches=()
    shopt -s nullglob
    matches=(/dev/ttyACM* /dev/ttyUSB* /dev/cu.usbmodem* /dev/cu.usbserial*)
    shopt -u nullglob
    if [[ "${#matches[@]}" -eq 1 ]]; then
        printf '%s\n' "${matches[0]}"
    elif [[ "${#matches[@]}" -gt 1 ]]; then
        echo "Multiple serial devices: ${matches[*]}. Use --port." >&2
        return 1
    else
        return 1
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--port)   PORT="$2"; shift 2 ;;
        -m|--monitor) RUN_MONITOR=1; shift ;;
        --build-only) BUILD_ONLY=1; shift ;;
        -h|--help)   usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage; exit 1 ;;
    esac
done

if [[ ! -f "${EXPORT_SCRIPT}" ]]; then
    echo "ESP-IDF export script not found: ${EXPORT_SCRIPT}" >&2
    echo "Set ESP_IDF_EXPORT_SCRIPT or install ESP-IDF 5.4.x." >&2
    exit 1
fi

if [[ -z "${PORT}" && "${BUILD_ONLY}" -eq 0 ]]; then
    PORT="$(detect_port || true)"
fi

echo "Board:          ${BOARD}"
echo "Target:         ${IDF_TARGET}"
echo "Build dir:      ${BUILD_DIR}"
echo "Using ESP-IDF:  ${EXPORT_SCRIPT}"
echo "Serial port:    ${PORT}"

cd "${SCRIPT_DIR}"

release_serial_port() {
    local port="$1"
    [[ -z "${port}" || ! -e "${port}" ]] && return 0
    command -v lsof >/dev/null 2>&1 || return 0
    local pids
    pids="$(lsof -t "${port}" 2>/dev/null || true)"
    if [[ -n "${pids}" ]]; then
        echo "Releasing serial port ${port} from PID(s): ${pids}" >&2
        # shellcheck disable=SC2086
        kill -9 ${pids} 2>/dev/null || true
        sleep 0.3
    fi
}

[[ "${BUILD_ONLY}" -eq 0 ]] && release_serial_port "${PORT}"

# ESP-IDF's export.sh derives the Python env name from the first `python3` on
# PATH. On hosts whose system Python is newer than the supported range this
# picks an env that install.sh never created. If the user did not choose one,
# select the newest installed IDF 5.4 env explicitly.
if [[ -z "${IDF_PYTHON_ENV_PATH:-}" ]]; then
    pick_env() {
        local env
        for env in "$@"; do
            if [[ -x "${env}/bin/python" ]]; then
                IDF_PYTHON_ENV_PATH="${env}"
            fi
        done
    }
    shopt -s nullglob
    pick_env "${HOME}"/.espressif/python_env/idf5.4_*_env
    if [[ -z "${IDF_PYTHON_ENV_PATH:-}" ]]; then
        pick_env "${HOME}"/.espressif/python_env/idf*_env
    fi
    shopt -u nullglob
    [[ -n "${IDF_PYTHON_ENV_PATH:-}" ]] && export IDF_PYTHON_ENV_PATH
fi

# shellcheck source=/dev/null
source "${EXPORT_SCRIPT}"

export IDF_TARGET="${IDF_TARGET}"

CMAKE_ARGS=(-B "${BUILD_DIR}" -DFLIPPER_BOARD="${BOARD}")
if [[ -f "sdkconfig.defaults.${BOARD}" ]]; then
    CMAKE_ARGS+=(-DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.${BOARD}")
fi

# Drop stale sdkconfig files that belong to a different target, otherwise
# set-target/reconfigure would inherit the wrong flash/PSRAM settings.
if [[ -f "sdkconfig" ]]; then
    CURRENT_TARGET="$(sed -n 's/^CONFIG_IDF_TARGET="\([^"]*\)"/\1/p' sdkconfig 2>/dev/null || true)"
    if [[ -n "${CURRENT_TARGET}" && "${CURRENT_TARGET}" != "${IDF_TARGET}" ]]; then
        echo "Removing stale root sdkconfig (target ${CURRENT_TARGET})."
        rm -f sdkconfig
    fi
fi
if [[ -f "${BUILD_DIR}/sdkconfig" ]]; then
    BUILD_TARGET="$(sed -n 's/^CONFIG_IDF_TARGET="\([^"]*\)"/\1/p' "${BUILD_DIR}/sdkconfig" 2>/dev/null || true)"
    if [[ -n "${BUILD_TARGET}" && "${BUILD_TARGET}" != "${IDF_TARGET}" ]]; then
        echo "Removing stale ${BUILD_DIR}/sdkconfig (target ${BUILD_TARGET})."
        rm -f "${BUILD_DIR}/sdkconfig"
    fi
fi

if [[ ! -f "${BUILD_DIR}/build.ninja" ]]; then
    echo "Setting IDF target to ${IDF_TARGET}..."
    idf.py "${CMAKE_ARGS[@]}" set-target "${IDF_TARGET}"
fi

if [[ "${BUILD_ONLY}" -eq 1 ]]; then
    idf.py "${CMAKE_ARGS[@]}" reconfigure build
    echo
    echo "Build complete (--build-only). Nothing flashed."
    exit 0
fi

if [[ -z "${PORT}" ]]; then
    echo "No serial device found. Use --port or ESPPORT." >&2
    exit 1
fi

idf.py "${CMAKE_ARGS[@]}" -p "${PORT}" reconfigure build flash

if [[ "${RUN_MONITOR}" -eq 1 ]]; then
    release_serial_port "${PORT}"
    idf.py -B "${BUILD_DIR}" -p "${PORT}" monitor
fi