#!/usr/bin/env bash
set -euo pipefail

# Run only inside the caller's existing exclusive host lease. Mounts keep host paths so
# source, incremental builds and test model paths agree inside/outside Docker.
image="${NINFER_BUILD_ENV_IMAGE:-ov-build-env:master-64d10fbf}"
holder="${NINFER_BUILD_HOLDER:?set NINFER_BUILD_HOLDER to your lease holder}"
name="ov-${holder}-env-$$"
cache="${NINFER_CCACHE_DIR:-${HOME}/.cache/ninfer-ccache/master-64d10fbf}"
args=(--rm --init --user "$(id -u):$(id -g)" --network host
      --ulimit memlock=-1 --env HOME=/tmp --env CCACHE_DIR=/ccache
      --env CCACHE_MAXSIZE=20G)
while (($#)); do
    case "$1" in
        --gpu) args+=(--gpus all); shift ;;
        --mount)
            mount="$(realpath "$2")"
            args+=(--mount "type=bind,source=${mount},target=${mount}")
            shift 2 ;;
        --read-only-mount)
            mount="$(realpath "$2")"
            args+=(--mount "type=bind,source=${mount},target=${mount},readonly")
            shift 2 ;;
        --env) args+=(--env "$2"); shift 2 ;;
        --seccomp)
            args+=(--security-opt "seccomp=$(realpath "$2")")
            shift 2 ;;
        --name) name="ov-${holder}-$2"; shift 2 ;;
        --) shift; break ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
if (($# == 0)); then
    echo "usage: NINFER_BUILD_HOLDER=holder run-in-env.sh [--gpu] [--mount dir] [--read-only-mount dir] [--env KEY=value] [--seccomp file] [--name suffix] -- command [args...]" >&2
    exit 2
fi
mkdir -p "$cache"
exec docker run "${args[@]}" --name "$name" \
    --mount "type=bind,source=${cache},target=/ccache" "$image" "$@"
