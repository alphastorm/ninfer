#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
  cat >&2 <<EOF
usage: $0 SOURCE_ROOT
EOF
  exit 2
}

[[ $# -eq 1 ]] || usage
source_root=$1

fail() {
  echo "$*" >&2
  exit 2
}

require_var() {
  local name=$1
  [[ -n ${!name:-} ]] || fail "profile toolchain environment is missing $name"
}

for name in \
  PROFILE_DOCKER_CLI PROFILE_DOCKER_CONTEXT PROFILE_DOCKER_ENDPOINT \
  PROFILE_DOCKER_DAEMON_ID PROFILE_TOOLCHAIN_BASE_IMAGE PROFILE_TOOLCHAIN_BASE_IMAGE_ID \
  PROFILE_TOOLCHAIN_IMAGE; do
  require_var "$name"
done

[[ -d $source_root ]] || fail "profile toolchain source root is not a directory: $source_root"
source_root=$(cd -- "$source_root" && pwd -P)
dockerfile=$source_root/tools/bench/Dockerfile.sm120-profile-python311
[[ -f $dockerfile ]] || fail "profile toolchain Dockerfile is missing: $dockerfile"
[[ -x $PROFILE_DOCKER_CLI ]] || fail "pinned Docker CLI is not executable: $PROFILE_DOCKER_CLI"
[[ $PROFILE_TOOLCHAIN_BASE_IMAGE_ID =~ ^sha256:[0-9a-f]{64}$ ]] || fail "PROFILE_TOOLCHAIN_BASE_IMAGE_ID must be an exact sha256 image ID"

python_version=3.11.11
python_source_sha256=2a9920c7a0cd236de33644ed980a13cbbc21058bfdc528febb6081575ed73be3
temporary_docker_config=$(mktemp -d)
trap 'rm -rf -- "$temporary_docker_config"' EXIT
printf '{}\n' >"$temporary_docker_config/config.json"
chmod 0600 "$temporary_docker_config/config.json"
export DOCKER_CONFIG=$temporary_docker_config
export DOCKER_CONTEXT=$PROFILE_DOCKER_CONTEXT
unset DOCKER_HOST

docker_cli() {
  "$PROFILE_DOCKER_CLI" --context "$PROFILE_DOCKER_CONTEXT" "$@"
}

endpoint=$(docker_cli context inspect "$PROFILE_DOCKER_CONTEXT" \
  --format '{{(index .Endpoints "docker").Host}}' 2>/dev/null) || fail "pinned Docker context is unavailable"
[[ $endpoint == "$PROFILE_DOCKER_ENDPOINT" ]] || fail "Docker endpoint differs from PROFILE_DOCKER_ENDPOINT"
daemon_id=$(docker_cli info --format '{{.ID}}' 2>/dev/null) || fail "pinned Docker daemon is unavailable"
[[ $daemon_id == "$PROFILE_DOCKER_DAEMON_ID" ]] || fail "Docker daemon differs from PROFILE_DOCKER_DAEMON_ID"
base_id=$(docker_cli image inspect "$PROFILE_TOOLCHAIN_BASE_IMAGE" --format '{{.Id}}' 2>/dev/null) || \
  fail "profile toolchain base image is unavailable locally: $PROFILE_TOOLCHAIN_BASE_IMAGE"
[[ $base_id == "$PROFILE_TOOLCHAIN_BASE_IMAGE_ID" ]] || fail "profile toolchain base image ID changed"

image_matches() {
  local image_id base_label version_label source_label
  image_id=$(docker_cli image inspect "$PROFILE_TOOLCHAIN_IMAGE" --format '{{.Id}}' 2>/dev/null) || return 1
  base_label=$(docker_cli image inspect "$image_id" --format '{{index .Config.Labels "org.ninfer.profile.base-image-id"}}' 2>/dev/null) || return 1
  version_label=$(docker_cli image inspect "$image_id" --format '{{index .Config.Labels "org.ninfer.profile.python-version"}}' 2>/dev/null) || return 1
  source_label=$(docker_cli image inspect "$image_id" --format '{{index .Config.Labels "org.ninfer.profile.python-source-sha256"}}' 2>/dev/null) || return 1
  [[ $base_label == "$PROFILE_TOOLCHAIN_BASE_IMAGE_ID" ]] || return 1
  [[ $version_label == "$python_version" ]] || return 1
  [[ $source_label == "$python_source_sha256" ]] || return 1
  # The command loop and Python assertion are evaluated by Bash inside the image.
  # shellcheck disable=SC2016
  docker_cli run --rm --pull never --network none --entrypoint /bin/bash "$image_id" -c \
    'set -e; for command in cmake ninja nvcc ncu nsys git sha256sum python3.11; do command -v "$command" >/dev/null; done; python3.11 -c '\''import sys; assert sys.version_info[:2] == (3, 11)'\''' \
    >/dev/null
  printf '%s\n' "$image_id"
}

if image_matches; then
  exit 0
fi
if docker_cli image inspect "$PROFILE_TOOLCHAIN_IMAGE" >/dev/null 2>&1; then
  fail "existing PROFILE_TOOLCHAIN_IMAGE does not match its pinned base and Python provenance"
fi

docker_cli build --pull=false --file "$dockerfile" --tag "$PROFILE_TOOLCHAIN_IMAGE" \
  --build-arg "BASE_IMAGE=$PROFILE_TOOLCHAIN_BASE_IMAGE" \
  --build-arg "BASE_IMAGE_ID=$PROFILE_TOOLCHAIN_BASE_IMAGE_ID" \
  --build-arg "PYTHON_VERSION=$python_version" \
  --build-arg "PYTHON_SOURCE_SHA256=$python_source_sha256" \
  "$source_root/tools/bench" >&2

base_id_after=$(docker_cli image inspect "$PROFILE_TOOLCHAIN_BASE_IMAGE" --format '{{.Id}}')
[[ $base_id_after == "$PROFILE_TOOLCHAIN_BASE_IMAGE_ID" ]] || fail "profile toolchain base image changed during build"
image_matches || fail "built profiling toolchain failed provenance or command validation"
