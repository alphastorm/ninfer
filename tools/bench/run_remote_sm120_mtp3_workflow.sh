#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
  cat >&2 <<EOF
usage: $0 {check|run} CONFIG
EOF
  exit 2
}

[[ $# -eq 2 && ( $1 == check || $1 == run ) ]] || usage
mode=$1
config=$2
[[ -f $config ]] || { echo "remote profile workflow config not found: $config" >&2; exit 2; }
config=$(cd -- "$(dirname -- "$config")" && pwd -P)/$(basename -- "$config")

set -a
# The configuration is a trusted, operator-owned shell environment file.
# shellcheck source=/dev/null
source "$config"
set +a

fail() {
  echo "$*" >&2
  exit 2
}

require_var() {
  local name=$1
  [[ -n ${!name:-} ]] || fail "remote profile workflow config is missing $name"
}

for name in \
  CONTROLLER_TARGET_HOST CONTROLLER_WSL_DISTRIBUTION CONTROLLER_OPERATION_PARENT \
  CONTROLLER_RECEIPT_ROOT CONTROLLER_WINDOWS_STAGE_DIR CONTROLLER_WSL_STAGE_DIR \
  PROFILE_UPSTREAM_BASE_SHA \
  PROFILE_DOCKER_CLI PROFILE_DOCKER_CONTEXT PROFILE_DOCKER_ENDPOINT \
  PROFILE_DOCKER_DAEMON_ID PROFILE_TOOLCHAIN_BASE_IMAGE PROFILE_TOOLCHAIN_BASE_IMAGE_ID \
  PROFILE_TOOLCHAIN_IMAGE \
  PROFILE_CONTAINER_PYTHON PROFILE_NCU_TIMEOUT_SECONDS PROFILE_NSYS_TIMEOUT_SECONDS \
  TOOLCHAIN_IMAGE CANDIDATE_PORT MODEL_PATH MODEL_SHA256 MODEL_ID \
  PROFILE_CONTROLLER_ID PROFILE_TARGET_ID \
  PRODUCTION_CONTAINER PRODUCTION_ID PRODUCTION_IMAGE PRODUCTION_RESTART_POLICY \
  PRODUCTION_PORT PRODUCTION_CONTROLLER ROLLBACK_CONTAINER ROLLBACK_ID ROLLBACK_IMAGE_ID \
  ROLLBACK_RUNNING ROLLBACK_RESTART_POLICY \
  PROFILE_MAX_CONTEXT PROFILE_PREFILL_CHUNK PROFILE_KV_DTYPE PROFILE_MAX_CONCURRENCY \
  PROFILE_DRAFT_TOKENS PROFILE_SEED PROFILE_STARTUP_TIMEOUT_SECONDS \
  PROFILE_PAYLOAD_STOP_TIMEOUT_SECONDS PROFILE_WARMUP_MESSAGES PROFILE_LONG_MESSAGES \
  PROFILE_DECODE_MESSAGES PROFILE_WARMUP_EXPECTED PROFILE_LONG_EXPECTED; do
  require_var "$name"
done

ssh_bin=${CONTROLLER_SSH_BIN:-ssh}
scp_bin=${CONTROLLER_SCP_BIN:-scp}
git_bin=${CONTROLLER_GIT_BIN:-git}
sha256_bin=${CONTROLLER_SHA256_BIN:-shasum}
source_root=${CONTROLLER_SOURCE_ROOT:-}
if [[ -z $source_root ]]; then
  source_root=$($git_bin -C "$(dirname -- "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)
fi
source_root=$(cd -- "$source_root" && pwd -P)
workflow_relative=tools/bench/run_sm120_mtp3_workflow.sh
toolchain_relative=tools/bench/ensure_sm120_profile_toolchain.sh
[[ -x $source_root/$workflow_relative ]] || fail "target workflow is not executable in controller source: $source_root/$workflow_relative"
[[ -x $source_root/$toolchain_relative ]] || fail "profile toolchain bootstrap is not executable in controller source: $source_root/$toolchain_relative"
for command in "$ssh_bin" "$scp_bin" "$git_bin" "$sha256_bin" mktemp tar; do
  command -v "$command" >/dev/null 2>&1 || fail "required controller command is unavailable: $command"
done
[[ $CONTROLLER_OPERATION_PARENT == /* ]] || fail "CONTROLLER_OPERATION_PARENT must be absolute"
[[ $CONTROLLER_RECEIPT_ROOT == /* ]] || fail "CONTROLLER_RECEIPT_ROOT must be absolute"
[[ $PROFILE_UPSTREAM_BASE_SHA =~ ^[0-9a-f]{40,64}$ ]] || fail "PROFILE_UPSTREAM_BASE_SHA must be a full lowercase Git SHA"

source_sha=$($git_bin -C "$source_root" rev-parse HEAD)
[[ $source_sha =~ ^[0-9a-f]{40,64}$ ]] || fail "controller source HEAD is not a full Git SHA"
source_status=$($git_bin -C "$source_root" status --porcelain --untracked-files=all)
[[ -z $source_status ]] || fail "controller source must be a completely clean committed worktree"
$git_bin -C "$source_root" cat-file -e "$PROFILE_UPSTREAM_BASE_SHA^{commit}" 2>/dev/null || fail "PROFILE_UPSTREAM_BASE_SHA is not in controller source"
$git_bin -C "$source_root" merge-base --is-ancestor "$PROFILE_UPSTREAM_BASE_SHA" "$source_sha" || fail "PROFILE_UPSTREAM_BASE_SHA is not an ancestor of controller source"

if [[ -n ${CONTROLLER_OPERATION_NAME:-} ]]; then
  operation_name=$CONTROLLER_OPERATION_NAME
else
  operation_name="ninfer-sm120-scripted-${source_sha:0:12}-$(date -u +%Y%m%dT%H%M%SZ)-$$"
fi
[[ $operation_name =~ ^[a-zA-Z0-9][a-zA-Z0-9_-]*$ ]] || fail "CONTROLLER_OPERATION_NAME contains unsupported characters"
operation_root=$CONTROLLER_OPERATION_PARENT/$operation_name
receipt_root=$CONTROLLER_RECEIPT_ROOT/$operation_name
[[ ! -e $receipt_root && ! -L $receipt_root ]] || fail "controller receipt path already exists: $receipt_root"

connection_options=(-o BatchMode=yes -o "ConnectTimeout=${CONTROLLER_SSH_CONNECT_TIMEOUT_SECONDS:-15}" \
  -o ServerAliveInterval=10 -o ServerAliveCountMax=6)
ssh_args=("$ssh_bin" "${connection_options[@]}")
scp_args=("$scp_bin" "${connection_options[@]}")
if [[ -n ${CONTROLLER_SSH_JUMP:-} ]]; then
  ssh_args+=(-J "$CONTROLLER_SSH_JUMP")
  scp_args+=(-o "ProxyJump=$CONTROLLER_SSH_JUMP")
fi
ssh_args+=("$CONTROLLER_TARGET_HOST")

remote_wsl() {
  "${ssh_args[@]}" wsl.exe -d "$CONTROLLER_WSL_DISTRIBUTION" -- "$@"
}

stage_file() {
  local source=$1
  local target=$2
  local name windows_target wsl_staging
  name="$operation_name-$(basename -- "$target")"
  windows_target=${CONTROLLER_WINDOWS_STAGE_DIR%/}/$name
  wsl_staging=${CONTROLLER_WSL_STAGE_DIR%/}/$name
  "${scp_args[@]}" "$source" "$CONTROLLER_TARGET_HOST:$windows_target"
  remote_wsl /usr/bin/install -m 0600 "$wsl_staging" "$target"
  remote_wsl /usr/bin/rm -f "$wsl_staging"
}

hash_file() {
  if [[ $sha256_bin == *shasum ]]; then
    "$sha256_bin" -a 256 "$1" | cut -d' ' -f1
  else
    "$sha256_bin" "$1" | cut -d' ' -f1
  fi
}

emit_value() {
  printf '%s=%q\n' "$1" "$2"
}

emit_config() {
  emit_value PROFILE_ROOT "$operation_root"
  emit_value PROFILE_SOURCE_ROOT "$operation_root/source"
  emit_value PROFILE_SOURCE_SHA "$source_sha"
  emit_value PROFILE_UPSTREAM_BASE_SHA "$PROFILE_UPSTREAM_BASE_SHA"
  emit_value PROFILE_BUILD_DIR "$operation_root/build/run"
  emit_value PROFILE_PREPARE_DIR "$operation_root/prepared/run"
  emit_value PROFILE_RESULT_DIR "$operation_root/results/run"
  emit_value PROFILE_DOCKER_CONFIG "$operation_root/docker-config/run"
  local name
  for name in \
    TOOLCHAIN_IMAGE PROFILE_TOOLCHAIN_IMAGE PROFILE_TOOLCHAIN_IMAGE_ID \
    PROFILE_CONTAINER_PYTHON PROFILE_DOCKER_CLI PROFILE_DOCKER_CONTEXT \
    PROFILE_DOCKER_ENDPOINT PROFILE_DOCKER_DAEMON_ID CANDIDATE_PORT \
    PRODUCTION_CONTAINER PRODUCTION_ID PRODUCTION_IMAGE PRODUCTION_RESTART_POLICY \
    PRODUCTION_PORT PRODUCTION_CONTROLLER ROLLBACK_CONTAINER ROLLBACK_ID ROLLBACK_IMAGE_ID \
    ROLLBACK_RUNNING ROLLBACK_RESTART_POLICY MODEL_PATH MODEL_SHA256 MODEL_ID \
    PROFILE_CONTROLLER_ID PROFILE_TARGET_ID PROFILE_MAX_CONTEXT PROFILE_PREFILL_CHUNK \
    PROFILE_KV_DTYPE PROFILE_MAX_CONCURRENCY PROFILE_DRAFT_TOKENS PROFILE_SEED \
    PROFILE_STARTUP_TIMEOUT_SECONDS PROFILE_PAYLOAD_STOP_TIMEOUT_SECONDS \
    PROFILE_NCU_TIMEOUT_SECONDS PROFILE_NSYS_TIMEOUT_SECONDS PROFILE_WARMUP_MESSAGES \
    PROFILE_LONG_MESSAGES PROFILE_DECODE_MESSAGES PROFILE_WARMUP_EXPECTED PROFILE_LONG_EXPECTED; do
    emit_value "$name" "${!name}"
  done
  emit_value PYTHON "${PYTHON:-/usr/bin/python3}"
  emit_value CANDIDATE_PREFIX "$operation_name-"
  emit_value PROFILE_DEPLOYMENT_PREFIX "$operation_name"
}

scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
bundle=$scratch/source.bundle
target_config=$scratch/profile.conf
$git_bin -C "$source_root" bundle create "$bundle" HEAD "$PROFILE_UPSTREAM_BASE_SHA"
$git_bin -C "$source_root" bundle verify "$bundle" >/dev/null
bundle_sha=$(hash_file "$bundle")

printf 'remote profile operation: %s\n' "$operation_name"
if ! remote_wsl /usr/bin/test ! -e "$operation_root"; then
  fail "remote operation path already exists: $operation_root"
fi
remote_wsl /usr/bin/mkdir -m 0700 "$operation_root"
stage_file "$bundle" "$operation_root/source.bundle"
remote_bundle_sha=$(remote_wsl /usr/bin/sha256sum "$operation_root/source.bundle" | cut -d' ' -f1)
[[ $remote_bundle_sha == "$bundle_sha" ]] || fail "remote source bundle SHA-256 differs from controller"
remote_wsl /usr/bin/git clone --no-checkout "$operation_root/source.bundle" "$operation_root/source"
remote_wsl /usr/bin/git -C "$operation_root/source" checkout --detach "$source_sha"
remote_head=$(remote_wsl /usr/bin/git -C "$operation_root/source" rev-parse HEAD)
[[ $remote_head == "$source_sha" ]] || fail "remote source HEAD differs from controller"
remote_status=$(remote_wsl /usr/bin/git -C "$operation_root/source" status --porcelain --untracked-files=all)
[[ -z $remote_status ]] || fail "remote source is not clean after staging"
remote_wsl /usr/bin/git -C "$operation_root/source" merge-base --is-ancestor "$PROFILE_UPSTREAM_BASE_SHA" "$source_sha"
remote_wsl /usr/bin/rm -f "$operation_root/source.bundle"

remote_toolchain=$operation_root/source/$toolchain_relative
PROFILE_TOOLCHAIN_IMAGE_ID=$(remote_wsl /usr/bin/env \
  "PROFILE_DOCKER_CLI=$PROFILE_DOCKER_CLI" \
  "PROFILE_DOCKER_CONTEXT=$PROFILE_DOCKER_CONTEXT" \
  "PROFILE_DOCKER_ENDPOINT=$PROFILE_DOCKER_ENDPOINT" \
  "PROFILE_DOCKER_DAEMON_ID=$PROFILE_DOCKER_DAEMON_ID" \
  "PROFILE_TOOLCHAIN_BASE_IMAGE=$PROFILE_TOOLCHAIN_BASE_IMAGE" \
  "PROFILE_TOOLCHAIN_BASE_IMAGE_ID=$PROFILE_TOOLCHAIN_BASE_IMAGE_ID" \
  "PROFILE_TOOLCHAIN_IMAGE=$PROFILE_TOOLCHAIN_IMAGE" \
  "$remote_toolchain" "$operation_root/source")
[[ $PROFILE_TOOLCHAIN_IMAGE_ID =~ ^sha256:[0-9a-f]{64}$ ]] || fail "profile toolchain bootstrap did not return an exact image ID"
emit_config >"$target_config"
config_sha=$(hash_file "$target_config")
stage_file "$target_config" "$operation_root/profile.conf"
remote_config_sha=$(remote_wsl /usr/bin/sha256sum "$operation_root/profile.conf" | cut -d' ' -f1)
[[ $remote_config_sha == "$config_sha" ]] || fail "remote profile config SHA-256 differs from controller"

remote_workflow=$operation_root/source/$workflow_relative
remote_config=$operation_root/profile.conf
remote_env=(/usr/bin/env "NINFER_CONTROLLER_ID=$PROFILE_CONTROLLER_ID" "NINFER_TARGET_ID=$PROFILE_TARGET_ID")
printf 'remote profile operation staged and verified: %s\n' "$operation_name"

set +e
remote_wsl "${remote_env[@]}" "$remote_workflow" "$mode" "$remote_config"
run_status=$?
set -e

mkdir -p "$receipt_root"
remote_wsl /usr/bin/cat "$remote_config" >"$receipt_root/profile.conf"
if remote_wsl /usr/bin/test -d "$operation_root/results/run"; then
  mkdir -p "$receipt_root/results"
  remote_wsl /usr/bin/tar -C "$operation_root/results/run" -cf - . | tar -C "$receipt_root/results" -xf -
fi
{
  printf 'operation_name\t%s\n' "$operation_name"
  printf 'mode\t%s\n' "$mode"
  printf 'operation_root\t%s\n' "$operation_root"
  printf 'source_sha\t%s\n' "$source_sha"
  printf 'upstream_base_sha\t%s\n' "$PROFILE_UPSTREAM_BASE_SHA"
  printf 'bundle_sha256\t%s\n' "$bundle_sha"
  printf 'config_sha256\t%s\n' "$config_sha"
  printf 'profile_toolchain_image_id\t%s\n' "$PROFILE_TOOLCHAIN_IMAGE_ID"
  printf 'remote_exit_status\t%s\n' "$run_status"
} >"$receipt_root/controller.tsv"

if [[ $mode == check ]]; then
  if ((run_status == 0)); then
    printf 'remote profile workflow check passed: operation=%s receipts=%s\n' "$operation_name" "$receipt_root"
    exit 0
  fi
  printf 'remote profile workflow check failed: operation=%s status=%s receipts=%s\n' \
    "$operation_name" "$run_status" "$receipt_root" >&2
  exit "$run_status"
fi

if ((run_status == 0)); then
  [[ $(<"$receipt_root/results/exit-status.txt") == 0 ]] || fail "successful remote workflow has a nonzero lease exit receipt"
  for path in \
    production-restored-at.txt production-final-status.txt production-final-inspect.txt \
    rollback-final-inspect.txt sm120-mtp3/ncu/counter-access-verified.txt \
    sm120-mtp3/packet-complete.txt; do
    [[ -f $receipt_root/results/$path ]] || fail "successful remote workflow is missing receipt: $path"
  done
  printf 'remote profile workflow complete: operation=%s receipts=%s\n' "$operation_name" "$receipt_root"
  exit 0
fi

if [[ -f $receipt_root/results/production-stop-requested-at.txt && ! -f $receipt_root/results/production-restored-at.txt ]]; then
  fail "remote workflow failed after requesting the production stop without a restoration receipt; use the maintained production controller"
fi
printf 'remote profile workflow failed before or after a verified restoration: operation=%s status=%s receipts=%s\n' \
  "$operation_name" "$run_status" "$receipt_root" >&2
exit "$run_status"
