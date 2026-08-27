#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
  cat >&2 <<EOF
usage: $0 check CONFIG [DOCKER_CONFIG]
       $0 run CONFIG COMMAND [ARG ...]
EOF
  exit 2
}

[[ $# -ge 2 ]] || usage
mode=$1
config=$2
shift 2
case "$mode" in
  check) [[ $# -le 1 ]] || usage ;;
  run) [[ $# -ge 1 ]] || usage ;;
  *) usage ;;
esac
docker_config_override=${1:-}
[[ $mode == run ]] || shift "$#"
[[ -f "$config" ]] || { echo "profile lease config not found: $config" >&2; exit 2; }
config=$(cd -- "$(dirname -- "$config")" && pwd)/$(basename -- "$config")
set -a
# The configuration is a trusted, operator-owned shell environment file.
# shellcheck source=/dev/null
source "$config"
set +a
# A trusted config may enable job control. Disable it so each background setsid process keeps a
# stable PID instead of making util-linux setsid fork a transient parent.
set +m

require_var() {
  local name=$1
  [[ -n ${!name:-} ]] || { echo "profile lease config is missing $name" >&2; exit 2; }
}

for name in \
  PROFILE_RESULT_DIR CANDIDATE_PREFIX CANDIDATE_PORT \
  PROFILE_DOCKER_CLI PROFILE_DOCKER_CONTEXT PROFILE_DOCKER_ENDPOINT \
  PROFILE_DOCKER_DAEMON_ID PROFILE_DOCKER_CONFIG \
  PROFILE_TOOLCHAIN_IMAGE_ID \
  PRODUCTION_CONTAINER PRODUCTION_ID PRODUCTION_IMAGE PRODUCTION_RESTART_POLICY \
  PRODUCTION_PORT PRODUCTION_CONTROLLER \
  ROLLBACK_CONTAINER ROLLBACK_ID ROLLBACK_IMAGE_ID ROLLBACK_RUNNING ROLLBACK_RESTART_POLICY; do
  require_var "$name"
done
[[ $PROFILE_TOOLCHAIN_IMAGE_ID =~ ^sha256:[0-9a-f]{64}$ ]] || {
  echo "PROFILE_TOOLCHAIN_IMAGE_ID must be an exact sha256 image ID" >&2
  exit 2
}

payload_stop_timeout=${PROFILE_PAYLOAD_STOP_TIMEOUT_SECONDS:-30}
if ! [[ $payload_stop_timeout =~ ^[0-9]+$ ]] || ((payload_stop_timeout < 1)); then
  echo "PROFILE_PAYLOAD_STOP_TIMEOUT_SECONDS must be a positive integer" >&2
  exit 2
fi
[[ -f $PRODUCTION_CONTROLLER && -x $PRODUCTION_CONTROLLER ]] || {
  echo "PRODUCTION_CONTROLLER is not an executable file: $PRODUCTION_CONTROLLER" >&2
  exit 2
}
for command in curl nvidia-smi python3 setsid; do
  command -v "$command" >/dev/null 2>&1 || {
    echo "$command is required by the profile lease" >&2
    exit 2
  }
done
[[ -x $PROFILE_DOCKER_CLI ]] || {
  echo "PROFILE_DOCKER_CLI is not executable: $PROFILE_DOCKER_CLI" >&2
  exit 2
}
docker_config=$PROFILE_DOCKER_CONFIG
if [[ $mode == check && -n $docker_config_override ]]; then
  docker_config=$docker_config_override
fi
[[ -d $docker_config && -f $docker_config/config.json ]] || {
  echo "operation-owned Docker config is missing: $docker_config/config.json" >&2
  exit 2
}
python3 - "$docker_config/config.json" <<'PY'
import json
import sys
from pathlib import Path

if json.loads(Path(sys.argv[1]).read_text(encoding="utf-8")) != {}:
    raise SystemExit("operation-owned Docker config must be an empty object")
PY
export DOCKER_CONFIG=$docker_config
export DOCKER_CONTEXT=$PROFILE_DOCKER_CONTEXT
unset DOCKER_HOST
docker_cli_dir=$(dirname -- "$PROFILE_DOCKER_CLI")
export PATH="$docker_cli_dir:$PATH"

docker_cli() {
  "$PROFILE_DOCKER_CLI" --context "$PROFILE_DOCKER_CONTEXT" "$@"
}

docker_endpoint=$(docker_cli context inspect "$PROFILE_DOCKER_CONTEXT" \
  --format '{{(index .Endpoints "docker").Host}}' 2>/dev/null) || {
  echo "pinned Docker Desktop Linux context is unavailable; preserve the configured WSL integration and Docker context" >&2
  exit 2
}
[[ $docker_endpoint == "$PROFILE_DOCKER_ENDPOINT" ]] || {
  echo "Docker endpoint differs from PROFILE_DOCKER_ENDPOINT" >&2
  exit 2
}
docker_daemon_id=$(docker_cli info --format '{{.ID}}' 2>/dev/null) || {
  echo "pinned Docker Desktop Linux daemon is unavailable; sign in interactively, start Docker Desktop, then use the maintained controller" >&2
  exit 2
}
[[ $docker_daemon_id == "$PROFILE_DOCKER_DAEMON_ID" ]] || {
  echo "Docker daemon differs from PROFILE_DOCKER_DAEMON_ID" >&2
  exit 2
}
docker_runtimes=$(docker_cli info --format '{{json .Runtimes}}' 2>/dev/null) || {
  echo "failed to inspect Docker runtimes on the pinned daemon" >&2
  exit 2
}
python3 - "$docker_runtimes" <<'PY'
import json
import sys

if "nvidia" not in json.loads(sys.argv[1]):
    raise SystemExit("pinned Docker Desktop Linux daemon has no nvidia runtime for --gpus all")
PY

[[ $CANDIDATE_PREFIX =~ ^[a-zA-Z0-9][a-zA-Z0-9_-]*-$ ]] || {
  echo "CANDIDATE_PREFIX must use alphanumeric, underscore, or hyphen characters and end in '-'" >&2
  exit 2
}
if ! [[ $CANDIDATE_PORT =~ ^[0-9]+$ ]] || ((CANDIDATE_PORT < 1 || CANDIDATE_PORT > 65535)); then
  echo "CANDIDATE_PORT must be in [1, 65535]" >&2
  exit 2
fi
if ! [[ $PRODUCTION_PORT =~ ^[0-9]+$ ]] || ((PRODUCTION_PORT < 1 || PRODUCTION_PORT > 65535)); then
  echo "PRODUCTION_PORT must be in [1, 65535]" >&2
  exit 2
fi

[[ ! -e $PROFILE_RESULT_DIR ]] || {
  echo "PROFILE_RESULT_DIR already exists: $PROFILE_RESULT_DIR" >&2
  exit 2
}

inspect() {
  local format=$1
  local container=$2
  docker_cli inspect -f "$format" "$container" 2>/dev/null
}

# Reached through the EXIT trap's restoration path.
# shellcheck disable=SC2329
remove_candidates() {
  local -a ids=()
  local -a remaining=()
  local id
  local listed
  listed=$(docker_cli ps -aq --filter "name=^/$CANDIDATE_PREFIX") || {
    echo "failed to inspect profiling candidates during restoration" >&2
    return 1
  }
  while IFS= read -r id; do
    [[ -n $id ]] && ids+=("$id")
  done <<<"$listed"
  if ((${#ids[@]})); then
    docker_cli rm --force "${ids[@]}" || true
  fi
  listed=$(docker_cli ps -aq --filter "name=^/$CANDIDATE_PREFIX") || {
    echo "failed to verify profiling candidate removal" >&2
    return 1
  }
  while IFS= read -r id; do
    [[ -n $id ]] && remaining+=("$id")
  done <<<"$listed"
  if ((${#remaining[@]})); then
    printf '%s\n' "${remaining[@]}" >"$PROFILE_RESULT_DIR/candidate-removal-failed.txt"
    echo "profiling candidates survived forced removal; production restart is blocked" >&2
    return 1
  fi
}

wait_for_health() {
  local port=$1
  local attempts=${2:-600}
  local attempt
  for ((attempt = 1; attempt <= attempts; ++attempt)); do
    if curl -fsS --max-time 2 "http://127.0.0.1:$port/health" >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  return 1
}

verify_production_identity() {
  [[ $(inspect '{{.Id}}' "$PRODUCTION_CONTAINER") == "$PRODUCTION_ID" ]] || return 1
  [[ $(inspect '{{.Config.Image}}' "$PRODUCTION_CONTAINER") == "$PRODUCTION_IMAGE" ]] || return 1
  [[ $(inspect '{{.HostConfig.RestartPolicy.Name}}' "$PRODUCTION_CONTAINER") == "$PRODUCTION_RESTART_POLICY" ]] || return 1
}

verify_rollback_identity() {
  [[ $(inspect '{{.Id}}' "$ROLLBACK_CONTAINER") == "$ROLLBACK_ID" ]] || return 1
  [[ $(inspect '{{.Image}}' "$ROLLBACK_CONTAINER") == "$ROLLBACK_IMAGE_ID" ]] || return 1
  [[ $(inspect '{{.State.Running}}' "$ROLLBACK_CONTAINER") == "$ROLLBACK_RUNNING" ]] || return 1
  [[ $(inspect '{{.HostConfig.RestartPolicy.Name}}' "$ROLLBACK_CONTAINER") == "$ROLLBACK_RESTART_POLICY" ]] || return 1
}

# Reached through on_exit.
# shellcheck disable=SC2329
restore_production() {
  set +e
  local rc=0
  local candidates_clear=true
  if ! remove_candidates; then
    candidates_clear=false
    rc=1
  fi
  if [[ $candidates_clear == true ]]; then
    if [[ $(inspect '{{.State.Running}}' "$PRODUCTION_CONTAINER") != true ]]; then
      "$PRODUCTION_CONTROLLER" restart || rc=1
    fi
    wait_for_health "$PRODUCTION_PORT" || rc=1
  fi
  verify_production_identity || rc=1
  [[ $(inspect '{{.State.Running}}' "$PRODUCTION_CONTAINER") == true ]] || rc=1
  verify_rollback_identity || rc=1
  "$PRODUCTION_CONTROLLER" status > "$PROFILE_RESULT_DIR/production-final-status.txt" 2>&1 || rc=1
  docker_cli inspect -f 'id={{.Id}} image={{.Config.Image}} running={{.State.Running}} restart={{.HostConfig.RestartPolicy.Name}} ports={{json .HostConfig.PortBindings}}' \
    "$PRODUCTION_CONTAINER" > "$PROFILE_RESULT_DIR/production-final-inspect.txt" 2>&1 || rc=1
  docker_cli inspect -f 'id={{.Id}} image={{.Image}} running={{.State.Running}} restart={{.HostConfig.RestartPolicy.Name}} ports={{json .HostConfig.PortBindings}}' \
    "$ROLLBACK_CONTAINER" > "$PROFILE_RESULT_DIR/rollback-final-inspect.txt" 2>&1 || rc=1
  if ((rc == 0)); then
    date -u +%Y-%m-%dT%H:%M:%SZ > "$PROFILE_RESULT_DIR/production-restored-at.txt"
  else
    date -u +%Y-%m-%dT%H:%M:%SZ > "$PROFILE_RESULT_DIR/production-restore-failed-at.txt"
  fi
  set -e
  return "$rc"
}

payload_pid=

payload_group_exists() {
  local pid=$1
  kill -0 -- "-$pid" 2>/dev/null
}

wait_for_payload_group_exit() {
  local pid=$1
  local attempts=$2
  local attempt
  for ((attempt = 0; attempt < attempts; ++attempt)); do
    if ! payload_group_exists "$pid"; then
      return 0
    fi
    sleep 0.05
  done
  ! payload_group_exists "$pid"
}

stop_payload_group() {
  local signal=$1
  local pid=$payload_pid
  local watchdog_pid=
  local rc=0
  [[ -n $pid ]] || return 0

  if payload_group_exists "$pid"; then
    setsid -- python3 - "$payload_stop_timeout" "$pid" <<'PY' &
import os
import signal
import sys
import time

time.sleep(int(sys.argv[1]))
try:
    os.killpg(int(sys.argv[2]), signal.SIGKILL)
except ProcessLookupError:
    pass
PY
    watchdog_pid=$!
    kill -s "$signal" -- "-$pid" 2>/dev/null || true
  fi

  wait "$pid" 2>/dev/null || true
  if [[ -n $watchdog_pid ]]; then
    if ! wait_for_payload_group_exit "$pid" "$((payload_stop_timeout * 20 + 20))"; then
      kill -KILL -- "-$pid" 2>/dev/null || true
      wait_for_payload_group_exit "$pid" 20 || rc=1
    fi
    kill -KILL -- "-$watchdog_pid" 2>/dev/null || kill -KILL "$watchdog_pid" 2>/dev/null || true
    wait "$watchdog_pid" 2>/dev/null || true
  fi
  payload_pid=
  return "$rc"
}

# Registered below as the EXIT trap.
# shellcheck disable=SC2329
on_exit() {
  local code=$?
  trap - EXIT
  trap '' HUP INT TERM
  if ! stop_payload_group TERM; then
    code=91
  fi
  rm -f -- "$PROFILE_RESULT_DIR/lease-active"
  if ! restore_production; then
    code=90
  fi
  printf '%s\n' "$code" > "$PROFILE_RESULT_DIR/exit-status.txt"
  exit "$code"
}

# A foreground Bash command defers trapped signals until that command returns. Run the payload in
# its own process group, then terminate and reap that whole group before restoring production.
# shellcheck disable=SC2329
terminate_payload_and_exit() {
  local signal=$1
  local code=$2
  trap '' HUP INT TERM
  printf '%s\t%s\n' "$signal" "$code" > "$PROFILE_RESULT_DIR/payload-signal.txt"
  if ! stop_payload_group "$signal"; then
    code=91
  fi
  exit "$code"
}

verify_production_identity || { echo "production identity differs from the lease config" >&2; exit 2; }
[[ $(inspect '{{.State.Running}}' "$PRODUCTION_CONTAINER") == true ]] || {
  echo "production is not running" >&2
  exit 2
}
wait_for_health "$PRODUCTION_PORT" 5 || { echo "production health check failed" >&2; exit 2; }
verify_rollback_identity || { echo "rollback container differs from the lease config" >&2; exit 2; }
candidate_ids=$(docker_cli ps -aq --filter "name=^/$CANDIDATE_PREFIX" 2>/dev/null) || {
  echo "failed to inspect profiling candidates" >&2
  exit 2
}
if [[ -n $candidate_ids ]]; then
  echo "a profiling candidate with prefix $CANDIDATE_PREFIX already exists" >&2
  exit 2
fi
python3 - "$CANDIDATE_PORT" <<'PY'
import socket
import sys

port = int(sys.argv[1])
with socket.socket() as sock:
    sock.bind(("127.0.0.1", port))
PY
if ! nvidia-smi --query-gpu=name,temperature.gpu,pstate,memory.used,memory.total --format=csv,noheader >/dev/null; then
  echo "host nvidia-smi query failed before the profile outage" >&2
  exit 2
fi
admission_name="${CANDIDATE_PREFIX}gpu-admission"
if ! docker_cli run --rm --pull never --name "$admission_name" --network none --gpus all \
  --entrypoint nvidia-smi "$PROFILE_TOOLCHAIN_IMAGE_ID" \
  --query-gpu=name,driver_version --format=csv,noheader >/dev/null; then
  docker_cli rm --force "$admission_name" >/dev/null 2>&1 || true
  echo "GPU container admission failed before the profile outage" >&2
  exit 2
fi

if [[ $mode == check ]]; then
  printf 'profile lease check passed: production=%s daemon=%s\n' "$PRODUCTION_ID" "$PROFILE_DOCKER_DAEMON_ID"
  exit 0
fi

mkdir -p "$PROFILE_RESULT_DIR"
: > "$PROFILE_RESULT_DIR/lease.log"
exec > >(tee -a "$PROFILE_RESULT_DIR/lease.log") 2>&1
sha256sum "$config" > "$PROFILE_RESULT_DIR/config.sha256"
sha256sum "$docker_config/config.json" > "$PROFILE_RESULT_DIR/docker-config.sha256"
{
  printf 'cli\t%s\n' "$PROFILE_DOCKER_CLI"
  printf 'context\t%s\n' "$PROFILE_DOCKER_CONTEXT"
  printf 'endpoint\t%s\n' "$docker_endpoint"
  printf 'daemon_id\t%s\n' "$docker_daemon_id"
  printf 'runtimes\t%s\n' "$docker_runtimes"
} >"$PROFILE_RESULT_DIR/docker-route.tsv"

trap on_exit EXIT
trap 'terminate_payload_and_exit HUP 129' HUP
trap 'terminate_payload_and_exit INT 130' INT
trap 'terminate_payload_and_exit TERM 143' TERM
date -u +%Y-%m-%dT%H:%M:%SZ > "$PROFILE_RESULT_DIR/production-stop-requested-at.txt"
docker_cli stop --time 30 "$PRODUCTION_CONTAINER" >/dev/null
date -u +%Y-%m-%dT%H:%M:%SZ > "$PROFILE_RESULT_DIR/production-stopped-at.txt"
[[ $(inspect '{{.State.Running}}' "$PRODUCTION_CONTAINER") == false ]] || {
  echo "production stop failed" >&2
  exit 3
}
nvidia-smi --query-gpu=name,temperature.gpu,pstate,memory.used,memory.total --format=csv,noheader \
  > "$PROFILE_RESULT_DIR/gpu-after-stop.txt"

printf '%s\n' "$$" >"$PROFILE_RESULT_DIR/lease-active"
chmod 0600 "$PROFILE_RESULT_DIR/lease-active"
export NINFER_PROFILE_LEASE_ACTIVE="$PROFILE_RESULT_DIR/lease-active"
set +e
setsid -- "$@" &
payload_pid=$!
wait "$payload_pid"
payload_status=$?
set -e
# The leader may have exited while descendants remain in the isolated process group. Terminate and
# observe that whole group before the EXIT trap restores production.
if ! stop_payload_group KILL; then
  payload_status=91
fi
date -u +%Y-%m-%dT%H:%M:%SZ > "$PROFILE_RESULT_DIR/gpu-work-completed-at.txt"
exit "$payload_status"
