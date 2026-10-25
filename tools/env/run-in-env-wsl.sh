#!/usr/bin/env bash
set -euo pipefail

here="$(dirname "$(realpath "$0")")"
export NINFER_DOCKER='/mnt/c/Program Files/Docker/Docker/resources/bin/docker.exe'
export NINFER_WINDOWS_DOCKER=1
export NINFER_DOCKER_CONFIG="${NINFER_DOCKER_CONFIG:-$here/docker-config}"
export NINFER_DOCKER_HOST="${NINFER_DOCKER_HOST:-$("$NINFER_DOCKER" context inspect --format '{{.Endpoints.docker.Host}}' | tr -d '\r')}"
export NINFER_BUILD_ENV_IMAGE="${NINFER_BUILD_ENV_IMAGE:-ov-build-env:master-64d10fbf-r2}"
export NINFER_CCACHE_DIR="${NINFER_CCACHE_DIR:-$here/ccache}"
exec "$here/run-in-env.sh" "$@"
