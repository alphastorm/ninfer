#!/usr/bin/env bash
set -euo pipefail

# Configure and build in the shared development image, under the caller's lease.
if (($# < 2)); then
    echo "usage: NINFER_BUILD_HOLDER=holder build.sh src-dir build-dir [cmake-args...] [-- targets...]" >&2
    exit 2
fi
src="$(realpath "$1")"
mkdir -p "$2"
build="$(realpath "$2")"
shift 2
configure=()
targets=()
while (($#)); do
    if [[ "$1" == -- ]]; then
        shift
        targets=("$@")
        break
    fi
    configure+=("$1")
    shift
done
runner="$(dirname "$(realpath "$0")")/run-in-env.sh"
if [[ -n "${NINFER_BUILD_SLOT_ROOT:-}" ]]; then
    mkdir -p "$NINFER_BUILD_SLOT_ROOT"
    exec 8>"$NINFER_BUILD_SLOT_ROOT/slot-0.lock"
    exec 9>"$NINFER_BUILD_SLOT_ROOT/slot-1.lock"
    while :; do
        if flock -n 8; then echo "build_slot=0"; break; fi
        if flock -n 9; then echo "build_slot=1"; break; fi
        sleep 1
    done
fi
"$runner" --mount "$src" --mount "$build" --env LD_LIBRARY_PATH=/opt/ninfer-driver-stub -- \
    cmake -S "$src" -B "$build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES=120a \
        -DNINFER_BUILD_APPS=ON -DBUILD_TESTING=ON \
        -DNINFER_BUILD_BENCHMARKS=OFF \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CUDA_COMPILER_LAUNCHER=ccache "${configure[@]}"
command=(cmake --build "$build" --parallel "${NINFER_BUILD_JOBS:-16}")
if ((${#targets[@]})); then
    command+=(--target "${targets[@]}")
fi
# Keep the slot-holding shell alive while Windows Docker runs through WSL interop.
"$runner" --mount "$src" --mount "$build" --env LD_LIBRARY_PATH=/opt/ninfer-driver-stub -- "${command[@]}"
