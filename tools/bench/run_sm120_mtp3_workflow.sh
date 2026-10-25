#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
  cat >&2 <<EOF
usage: $0 check CONFIG
       $0 run CONFIG
EOF
  exit 2
}

[[ $# -eq 2 ]] || usage
mode=$1
config=$2
inherited_lease_active=${NINFER_PROFILE_LEASE_ACTIVE:-}
case "$mode" in
  check|run) ;;
  payload) [[ -n $inherited_lease_active ]] || usage ;;
  *) usage ;;
esac
[[ -f $config ]] || { echo "profile workflow config not found: $config" >&2; exit 2; }
config=$(cd -- "$(dirname -- "$config")" && pwd -P)/$(basename -- "$config")

set -a
# The configuration is a trusted, operator-owned shell environment file.
# shellcheck source=/dev/null
source "$config"
set +a
if [[ $mode == payload ]]; then
  export NINFER_PROFILE_LEASE_ACTIVE=$inherited_lease_active
fi

fail() {
  echo "$*" >&2
  exit 2
}

require_var() {
  local name=$1
  [[ -n ${!name:-} ]] || fail "profile workflow config is missing $name"
}

for name in \
  PROFILE_ROOT PROFILE_SOURCE_ROOT PROFILE_BUILD_DIR PROFILE_PREPARE_DIR PROFILE_RESULT_DIR \
  PROFILE_SOURCE_SHA PROFILE_UPSTREAM_BASE_SHA \
  PROFILE_DOCKER_CLI PROFILE_DOCKER_CONTEXT PROFILE_DOCKER_ENDPOINT \
  PROFILE_DOCKER_DAEMON_ID PROFILE_DOCKER_CONFIG PROFILE_TOOLCHAIN_IMAGE PROFILE_TOOLCHAIN_IMAGE_ID \
  PROFILE_CONTAINER_PYTHON PROFILE_NCU_TIMEOUT_SECONDS PROFILE_NSYS_TIMEOUT_SECONDS \
  TOOLCHAIN_IMAGE CANDIDATE_PREFIX CANDIDATE_PORT MODEL_PATH MODEL_SHA256 \
  PROFILE_CONTROLLER_ID PROFILE_TARGET_ID \
  PRODUCTION_CONTAINER PRODUCTION_ID PRODUCTION_IMAGE PRODUCTION_RESTART_POLICY \
  PRODUCTION_PORT PRODUCTION_CONTROLLER ROLLBACK_CONTAINER ROLLBACK_ID ROLLBACK_IMAGE_ID \
  ROLLBACK_RUNNING ROLLBACK_RESTART_POLICY; do
  require_var "$name"
done
model_path_name=MODEL_PATH
model_path_config=${!model_path_name}

python=${PYTHON:-python3.11}
for command in git sha256sum "$python"; do
  command -v "$command" >/dev/null 2>&1 || fail "required workflow command is unavailable: $command"
done
[[ -x $PROFILE_DOCKER_CLI ]] || fail "pinned Docker CLI is not executable: $PROFILE_DOCKER_CLI"
[[ $PROFILE_SOURCE_SHA =~ ^[0-9a-f]{40,64}$ ]] || fail "PROFILE_SOURCE_SHA must be a full lowercase Git SHA"
[[ $PROFILE_UPSTREAM_BASE_SHA =~ ^[0-9a-f]{40,64}$ ]] || \
  fail "PROFILE_UPSTREAM_BASE_SHA must be a full lowercase Git SHA"
[[ $MODEL_SHA256 =~ ^[0-9a-f]{64}$ ]] || fail "MODEL_SHA256 must be a lowercase SHA-256"
[[ $PROFILE_TOOLCHAIN_IMAGE_ID =~ ^sha256:[0-9a-f]{64}$ ]] || fail "PROFILE_TOOLCHAIN_IMAGE_ID must be an exact sha256 image ID"
[[ $CANDIDATE_PREFIX =~ ^[a-zA-Z0-9][a-zA-Z0-9_-]*-$ ]] || \
  fail "CANDIDATE_PREFIX must use alphanumeric, underscore, or hyphen characters and end in '-'"
[[ $PROFILE_CONTROLLER_ID != "$PROFILE_TARGET_ID" ]] || fail "controller and target identities must differ"
[[ -x $PRODUCTION_CONTROLLER ]] || fail "PRODUCTION_CONTROLLER is not executable: $PRODUCTION_CONTROLLER"
[[ $PROFILE_TOOLCHAIN_IMAGE != "$TOOLCHAIN_IMAGE" && $PROFILE_TOOLCHAIN_IMAGE_ID != "$TOOLCHAIN_IMAGE" ]] || fail "profiling and runtime candidate images must be distinct"
for timeout_name in PROFILE_NCU_TIMEOUT_SECONDS PROFILE_NSYS_TIMEOUT_SECONDS; do
  timeout_value=${!timeout_name}
  if ! [[ $timeout_value =~ ^[0-9]+$ ]] || ((timeout_value < 1)); then
    fail "$timeout_name must be a positive integer"
  fi
done

for path_name in PROFILE_ROOT PROFILE_SOURCE_ROOT PROFILE_BUILD_DIR PROFILE_PREPARE_DIR \
                 PROFILE_RESULT_DIR PROFILE_DOCKER_CONFIG MODEL_PATH; do
  [[ ${!path_name} == /* ]] || fail "$path_name must be an absolute path"
done
case "$PROFILE_BUILD_DIR" in "$PROFILE_ROOT"/*) ;; *) fail "PROFILE_BUILD_DIR must be below PROFILE_ROOT" ;; esac
case "$PROFILE_PREPARE_DIR" in "$PROFILE_ROOT"/*) ;; *) fail "PROFILE_PREPARE_DIR must be below PROFILE_ROOT" ;; esac
case "$PROFILE_RESULT_DIR" in "$PROFILE_ROOT"/*) ;; *) fail "PROFILE_RESULT_DIR must be below PROFILE_ROOT" ;; esac
case "$PROFILE_DOCKER_CONFIG" in "$PROFILE_ROOT"/*) ;; *) fail "PROFILE_DOCKER_CONFIG must be below PROFILE_ROOT" ;; esac

[[ -d $PROFILE_SOURCE_ROOT ]] || fail "PROFILE_SOURCE_ROOT is not a directory: $PROFILE_SOURCE_ROOT"
[[ -f $model_path_config ]] || fail "MODEL_PATH is not a regular file: $model_path_config"
source_root=$(cd -- "$PROFILE_SOURCE_ROOT" && pwd -P)
model_path=$(cd -- "$(dirname -- "$model_path_config")" && pwd -P)/$(basename -- "$model_path_config")
workflow=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/$(basename -- "${BASH_SOURCE[0]}")
expected_workflow="$source_root/tools/bench/run_sm120_mtp3_workflow.sh"
lease_runner="$source_root/tools/bench/run_gpu_profile_lease.sh"
profile_runner="$source_root/tools/bench/run_sm120_mtp3_profile.sh"
[[ $workflow == "$expected_workflow" ]] || fail "workflow script is not from PROFILE_SOURCE_ROOT"
[[ -x $lease_runner ]] || fail "profile lease runner is not executable: $lease_runner"
[[ -x $profile_runner ]] || fail "SM120 profile runner is not executable: $profile_runner"

source_sha=$(git -C "$source_root" rev-parse HEAD)
[[ $source_sha == "$PROFILE_SOURCE_SHA" ]] || fail "PROFILE_SOURCE_ROOT does not match PROFILE_SOURCE_SHA"
source_status=$(git -C "$source_root" status --porcelain --untracked-files=all)
[[ -z $source_status ]] || fail "profiling source must be a completely clean committed worktree"
git -C "$source_root" cat-file -e "$PROFILE_UPSTREAM_BASE_SHA^{commit}" 2>/dev/null || fail "PROFILE_UPSTREAM_BASE_SHA is not a commit in PROFILE_SOURCE_ROOT"
git -C "$source_root" merge-base --is-ancestor "$PROFILE_UPSTREAM_BASE_SHA" "$source_sha" || fail "PROFILE_UPSTREAM_BASE_SHA is not an ancestor of PROFILE_SOURCE_SHA"
model_sha=$(sha256sum "$model_path" | cut -d' ' -f1)
[[ $model_sha == "$MODEL_SHA256" ]] || fail "MODEL_PATH does not match MODEL_SHA256"

export DOCKER_CONTEXT=$PROFILE_DOCKER_CONTEXT
unset DOCKER_HOST

docker_cli() {
  "$PROFILE_DOCKER_CLI" --context "$PROFILE_DOCKER_CONTEXT" "$@"
}

verify_empty_docker_config() {
  [[ -d $DOCKER_CONFIG && -f $DOCKER_CONFIG/config.json ]] || \
    fail "operation-owned Docker config is missing: $DOCKER_CONFIG/config.json"
  "$python" - "$DOCKER_CONFIG/config.json" <<'PY'
import json
import sys
from pathlib import Path

value = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
if value != {}:
    raise SystemExit("operation-owned Docker config must be an empty object")
PY
}

verify_docker_route() {
  local endpoint daemon_id image_id runtime_image_id
  endpoint=$(docker_cli context inspect "$PROFILE_DOCKER_CONTEXT" \
    --format '{{(index .Endpoints "docker").Host}}' 2>/dev/null) || {
    fail "pinned Docker Desktop Linux context is unavailable; preserve the configured WSL integration and Docker context"
  }
  [[ $endpoint == "$PROFILE_DOCKER_ENDPOINT" ]] || \
    fail "Docker endpoint differs from PROFILE_DOCKER_ENDPOINT"
  daemon_id=$(docker_cli info --format '{{.ID}}' 2>/dev/null) || {
    fail "pinned Docker Desktop Linux daemon is unavailable; sign in interactively, start Docker Desktop, then use the maintained controller"
  }
  [[ $daemon_id == "$PROFILE_DOCKER_DAEMON_ID" ]] || \
    fail "Docker daemon differs from PROFILE_DOCKER_DAEMON_ID"
  image_id=$(docker_cli image inspect "$PROFILE_TOOLCHAIN_IMAGE" --format '{{.Id}}' 2>/dev/null) || \
    fail "pinned profiling toolchain image is unavailable locally: $PROFILE_TOOLCHAIN_IMAGE"
  [[ $image_id == "$PROFILE_TOOLCHAIN_IMAGE_ID" ]] || fail "PROFILE_TOOLCHAIN_IMAGE differs from PROFILE_TOOLCHAIN_IMAGE_ID"
  runtime_image_id=$(docker_cli image inspect "$TOOLCHAIN_IMAGE" --format '{{.Id}}' 2>/dev/null) || \
    fail "pinned runtime candidate image is unavailable locally: $TOOLCHAIN_IMAGE"
  [[ $runtime_image_id != "$PROFILE_TOOLCHAIN_IMAGE_ID" ]] || \
    fail "profiling and runtime candidate images resolve to the same image ID"
}

verify_new_operation_paths() {
  local path
  for path in "$PROFILE_BUILD_DIR" "$PROFILE_PREPARE_DIR" "$PROFILE_RESULT_DIR" \
              "$PROFILE_DOCKER_CONFIG"; do
    [[ ! -e $path && ! -L $path ]] || fail "profile operation path already exists: $path"
  done
}

candidate_prepare="${CANDIDATE_PREFIX}prepare"
candidate_payload="${CANDIDATE_PREFIX}sm120-mtp3"
verify_candidate_names_free() {
  local ids
  ids=$(docker_cli ps -aq --filter "name=^/${CANDIDATE_PREFIX}" 2>/dev/null) || \
    fail "failed to inspect profiling container prefix"
  [[ -z $ids ]] || fail "a container already uses CANDIDATE_PREFIX"
}

run_prepare() {
  mkdir -p "$PROFILE_BUILD_DIR" "$PROFILE_PREPARE_DIR"
  chmod 0700 "$PROFILE_BUILD_DIR" "$PROFILE_PREPARE_DIR"
  verify_candidate_names_free
  docker_cli run --rm --pull never --name "$candidate_prepare" --network none \
    --mount "type=bind,source=$source_root,target=/workspace/source,readonly" \
    --mount "type=bind,source=$PROFILE_BUILD_DIR,target=/workspace/build" \
    --mount "type=bind,source=$model_path,target=/workspace/model.ninfer,readonly" \
    --mount "type=bind,source=$PROFILE_PREPARE_DIR,target=/workspace/prepare" \
    --workdir /workspace/source \
    --env GIT_CONFIG_COUNT=1 \
    --env GIT_CONFIG_KEY_0=safe.directory \
    --env GIT_CONFIG_VALUE_0=/workspace/source \
    --env NINFER_EXPECTED_SOURCE_SHA="$PROFILE_SOURCE_SHA" \
    --env NINFER_UPSTREAM_BASE_SHA="$PROFILE_UPSTREAM_BASE_SHA" \
    --env NINFER_ARTIFACT_EXPECTED_SHA256="$MODEL_SHA256" \
    --env NINFER_CONTROLLER_ID="$PROFILE_CONTROLLER_ID" \
    --env NINFER_TARGET_ID="$PROFILE_TARGET_ID" \
    --env NINFER_TOOLCHAIN_IMAGE_ID="$PROFILE_TOOLCHAIN_IMAGE_ID" \
    --env PYTHON="$PROFILE_CONTAINER_PYTHON" \
    --entrypoint /bin/bash \
    "$PROFILE_TOOLCHAIN_IMAGE_ID" \
    /workspace/source/tools/bench/run_sm120_mtp3_profile.sh prepare \
      /workspace/source /workspace/build /workspace/model.ninfer /workspace/prepare
  {
    printf 'source_sha\t%s\n' "$PROFILE_SOURCE_SHA"
    printf 'upstream_base_sha\t%s\n' "$PROFILE_UPSTREAM_BASE_SHA"
    printf 'model_sha256\t%s\n' "$MODEL_SHA256"
    printf 'docker_cli\t%s\n' "$PROFILE_DOCKER_CLI"
    printf 'docker_context\t%s\n' "$PROFILE_DOCKER_CONTEXT"
    printf 'docker_endpoint\t%s\n' "$PROFILE_DOCKER_ENDPOINT"
    printf 'docker_daemon_id\t%s\n' "$PROFILE_DOCKER_DAEMON_ID"
    printf 'toolchain_image\t%s\n' "$PROFILE_TOOLCHAIN_IMAGE"
    printf 'toolchain_image_id\t%s\n' "$PROFILE_TOOLCHAIN_IMAGE_ID"
  } >"$PROFILE_PREPARE_DIR/workflow-preflight.tsv"
}

run_payload() {
  [[ ${NINFER_PROFILE_LEASE_ACTIVE:-} == "$PROFILE_RESULT_DIR/lease-active" && -f $NINFER_PROFILE_LEASE_ACTIVE ]] || fail "the SM120 payload may run only inside an active profile lease"
  lease_pid=$(<"$NINFER_PROFILE_LEASE_ACTIVE")
  if ! [[ $lease_pid =~ ^[0-9]+$ ]] || ! kill -0 "$lease_pid" 2>/dev/null; then
    fail "the profile lease process is not active"
  fi
  [[ $lease_pid == "$PPID" ]] || fail "the SM120 payload is not owned by its active profile lease"
  [[ $(docker_cli inspect -f '{{.Id}}' "$PRODUCTION_CONTAINER" 2>/dev/null) == "$PRODUCTION_ID" ]] || fail "production identity changed before the profile payload"
  [[ $(docker_cli inspect -f '{{.State.Running}}' "$PRODUCTION_CONTAINER" 2>/dev/null) == false ]] || fail "production must be stopped by the active lease before GPU capture"
  [[ -f $PROFILE_PREPARE_DIR/prepare-complete.txt ]] || fail "profiling preparation is incomplete"
  verify_candidate_names_free
  docker_cli run --rm --pull never --name "$candidate_payload" --network none \
    --gpus all --cap-add SYS_ADMIN --security-opt seccomp=unconfined \
    --mount "type=bind,source=$source_root,target=/workspace/source,readonly" \
    --mount "type=bind,source=$PROFILE_BUILD_DIR,target=/workspace/build,readonly" \
    --mount "type=bind,source=$model_path,target=/workspace/model.ninfer,readonly" \
    --mount "type=bind,source=$PROFILE_PREPARE_DIR,target=/workspace/prepare,readonly" \
    --mount "type=bind,source=$PROFILE_RESULT_DIR,target=/workspace/result" \
    --workdir /workspace/source \
    --env GIT_CONFIG_COUNT=1 \
    --env GIT_CONFIG_KEY_0=safe.directory \
    --env GIT_CONFIG_VALUE_0=/workspace/source \
    --env NINFER_EXPECTED_SOURCE_SHA="$PROFILE_SOURCE_SHA" \
    --env NINFER_UPSTREAM_BASE_SHA="$PROFILE_UPSTREAM_BASE_SHA" \
    --env NINFER_ARTIFACT_EXPECTED_SHA256="$MODEL_SHA256" \
    --env NINFER_CONTROLLER_ID="$PROFILE_CONTROLLER_ID" \
    --env NINFER_TARGET_ID="$PROFILE_TARGET_ID" \
    --env NINFER_TOOLCHAIN_IMAGE_ID="$PROFILE_TOOLCHAIN_IMAGE_ID" \
    --env NINFER_NCU_TIMEOUT_SECONDS="${PROFILE_NCU_TIMEOUT_SECONDS:-180}" \
    --env NINFER_NSYS_TIMEOUT_SECONDS="${PROFILE_NSYS_TIMEOUT_SECONDS:-300}" \
    --env PYTHON="$PROFILE_CONTAINER_PYTHON" \
    --entrypoint /bin/bash \
    "$PROFILE_TOOLCHAIN_IMAGE_ID" \
    /workspace/source/tools/bench/run_sm120_mtp3_profile.sh capture \
      /workspace/source /workspace/build /workspace/model.ninfer \
      /workspace/prepare /workspace/result/sm120-mtp3
}

create_empty_docker_config() {
  local directory=$1
  mkdir -p "$directory"
  chmod 0700 "$directory"
  printf '{}\n' >"$directory/config.json"
  chmod 0600 "$directory/config.json"
}

case "$mode" in
  check)
    verify_new_operation_paths
    temporary_docker_config=$(mktemp -d)
    trap 'rm -rf -- "$temporary_docker_config"' EXIT
    create_empty_docker_config "$temporary_docker_config"
    export DOCKER_CONFIG=$temporary_docker_config
    verify_empty_docker_config
    verify_docker_route
    verify_candidate_names_free
    "$lease_runner" check "$config" "$temporary_docker_config"
    printf 'profile workflow check passed: source=%s daemon=%s image=%s\n' \
      "$PROFILE_SOURCE_SHA" "$PROFILE_DOCKER_DAEMON_ID" "$PROFILE_TOOLCHAIN_IMAGE_ID"
    ;;
  run)
    verify_new_operation_paths
    temporary_docker_config=$(mktemp -d)
    trap 'rm -rf -- "$temporary_docker_config"' EXIT
    create_empty_docker_config "$temporary_docker_config"
    export DOCKER_CONFIG=$temporary_docker_config
    verify_empty_docker_config
    verify_docker_route
    "$lease_runner" check "$config" "$temporary_docker_config"
    rm -rf -- "$temporary_docker_config"
    trap - EXIT
    create_empty_docker_config "$PROFILE_DOCKER_CONFIG"
    export DOCKER_CONFIG=$PROFILE_DOCKER_CONFIG
    verify_empty_docker_config
    verify_docker_route
    run_prepare
    NINFER_CONTROLLER_ID=$PROFILE_CONTROLLER_ID \
    NINFER_TARGET_ID=$PROFILE_TARGET_ID \
      "$lease_runner" run "$config" "$workflow" payload "$config"
    ;;
  payload)
    export DOCKER_CONFIG=$PROFILE_DOCKER_CONFIG
    verify_empty_docker_config
    verify_docker_route
    run_payload
    ;;
esac
