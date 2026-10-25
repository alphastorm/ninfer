#!/usr/bin/env bash
set -euo pipefail

# Run only inside the caller's existing exclusive host lease. Mounts keep host paths so
# source, incremental builds and test model paths agree inside/outside Docker.
image="${NINFER_BUILD_ENV_IMAGE:-ov-build-env:master-64d10fbf}"
holder="${NINFER_BUILD_HOLDER:?set NINFER_BUILD_HOLDER to your lease holder}"
name="ov-${holder}-env-$$"
cache="${NINFER_CCACHE_DIR:-${HOME}/.cache/ninfer-ccache/master-64d10fbf}"
docker="${NINFER_DOCKER:-docker}"
docker_args=()
host_path() {
    if [[ "${NINFER_WINDOWS_DOCKER:-0}" == 1 ]]; then
        if [[ "$1" != /mnt/[a-z]/* ]]; then
            echo "Windows Docker requires a /mnt/<drive>/ path, not an unintegrated WSL filesystem: $1" >&2
            return 2
        fi
        wslpath -w "$1"
    else
        printf '%s\n' "$1"
    fi
}
if [[ -n "${NINFER_DOCKER_CONFIG:-}" ]]; then
    mkdir -p "$NINFER_DOCKER_CONFIG"
    docker_args+=(--config "$(host_path "$NINFER_DOCKER_CONFIG")")
fi
if [[ -n "${NINFER_DOCKER_HOST:-}" ]]; then
    docker_args+=(--host "$NINFER_DOCKER_HOST")
fi
args=(--rm --init --user "$(id -u):$(id -g)"
      --ulimit memlock=-1 --env HOME=/tmp --env CCACHE_DIR=/ccache
      --env CCACHE_MAXSIZE=20G)
while (($#)); do
    case "$1" in
        --gpu) args+=(--gpus all); shift ;;
        --publish) args+=(--publish "127.0.0.1:$2:$2"); shift 2 ;;
        --volume)
            args+=(--mount "type=volume,source=$2,target=$3")
            shift 3 ;;
        --mount)
            mount="$(realpath "$2")"
            source="$(host_path "$mount")"
            args+=(--mount "type=bind,source=${source},target=${mount}")
            shift 2 ;;
        --read-only-mount)
            mount="$(realpath "$2")"
            source="$(host_path "$mount")"
            args+=(--mount "type=bind,source=${source},target=${mount},readonly")
            shift 2 ;;
        --env) args+=(--env "$2"); shift 2 ;;
        --seccomp)
            profile="$(host_path "$(realpath "$2")")"
            args+=(--security-opt "seccomp=${profile}")
            shift 2 ;;
        --name) name="ov-${holder}-$2"; shift 2 ;;
        --) shift; break ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
if (($# == 0)); then
    echo "usage: NINFER_BUILD_HOLDER=holder run-in-env.sh [--gpu] [--publish port] [--volume name target] [--mount dir] [--read-only-mount dir] [--env KEY=value] [--seccomp file] [--name suffix] -- command [args...]" >&2
    exit 2
fi
mkdir -p "$cache"
cache_source="$(host_path "$cache")"
create_output="$(mktemp "$cache/container-id.XXXXXX")"
create_pid=
client_pid=
container_id=
cleanup() {
    status=$?
    trap - EXIT
    trap '' INT TERM
    # Windows Docker does not die with its WSL client. Finish creation before
    # cleanup so a signal cannot race a later, unobserved container start.
    if [[ -n "$create_pid" ]]; then
        wait "$create_pid" || true
    elif [[ -z "$container_id" ]]; then
        # A trap may run between spawning create and assigning its PID.
        wait || true
    fi
    if [[ -z "$container_id" ]]; then
        candidate="$(tr -d '\r\n' < "$create_output")"
        if [[ "$candidate" =~ ^[0-9a-f]{64}$ ]]; then container_id="$candidate"; fi
    fi
    if [[ -n "$container_id" ]]; then
        # Keep the caller's build/GPU flock held until this exact container has
        # stopped or the daemon confirms that auto-removal already completed.
        while ! "$docker" "${docker_args[@]}" stop --time 15 "$container_id" >/dev/null 2>&1; do
            if remaining="$("$docker" "${docker_args[@]}" container ls --all --quiet \
                --filter "id=$container_id")"; then
                [[ -z "$remaining" ]] && break
            fi
            sleep 1
        done
        if [[ -n "$client_pid" ]]; then wait "$client_pid" || true; fi
        "$docker" "${docker_args[@]}" rm "$container_id" >/dev/null 2>&1 || true
    fi
    wait || true
    rm -f -- "$create_output"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
"$docker" "${docker_args[@]}" create "${args[@]}" --name "$name" \
    --mount "type=bind,source=${cache_source},target=/ccache" "$image" "$@" >"$create_output" &
create_pid=$!
wait "$create_pid"
container_id="$(tr -d '\r\n' < "$create_output")"
create_pid=
if [[ ! "$container_id" =~ ^[0-9a-f]{64}$ ]]; then
    echo "Docker create did not return a container ID" >&2
    exit 1
fi
"$docker" "${docker_args[@]}" start --attach "$container_id" &
client_pid=$!
wait "$client_pid"
