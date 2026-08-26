#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
  echo "usage: $0 CONFIG COMMAND [ARG ...]" >&2
  exit 2
}

[[ $# -ge 2 ]] || usage
config=$1
shift
[[ -f "$config" ]] || { echo "profile lease config not found: $config" >&2; exit 2; }
config=$(cd -- "$(dirname -- "$config")" && pwd)/$(basename -- "$config")
set -a
# The configuration is a trusted, operator-owned shell environment file.
# shellcheck source=/dev/null
source "$config"
set +a

require_var() {
  local name=$1
  [[ -n ${!name:-} ]] || { echo "profile lease config is missing $name" >&2; exit 2; }
}

for name in \
  PROFILE_RESULT_DIR CANDIDATE_PREFIX CANDIDATE_PORT \
  PRODUCTION_CONTAINER PRODUCTION_ID PRODUCTION_IMAGE PRODUCTION_RESTART_POLICY \
  PRODUCTION_PORT PRODUCTION_CONTROLLER \
  ROLLBACK_CONTAINER ROLLBACK_ID ROLLBACK_IMAGE_ID ROLLBACK_RUNNING ROLLBACK_RESTART_POLICY; do
  require_var "$name"
done

payload_stop_timeout=${PROFILE_PAYLOAD_STOP_TIMEOUT_SECONDS:-30}
if ! [[ $payload_stop_timeout =~ ^[0-9]+$ ]] || ((payload_stop_timeout < 1)); then
  echo "PROFILE_PAYLOAD_STOP_TIMEOUT_SECONDS must be a positive integer" >&2
  exit 2
fi
[[ -f $PRODUCTION_CONTROLLER && -x $PRODUCTION_CONTROLLER ]] || {
  echo "PRODUCTION_CONTROLLER is not an executable file: $PRODUCTION_CONTROLLER" >&2
  exit 2
}
command -v setsid >/dev/null 2>&1 || { echo "setsid is required to isolate the payload" >&2; exit 2; }

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
mkdir -p "$PROFILE_RESULT_DIR"
: > "$PROFILE_RESULT_DIR/lease.log"
exec > >(tee -a "$PROFILE_RESULT_DIR/lease.log") 2>&1
sha256sum "$config" > "$PROFILE_RESULT_DIR/config.sha256"

inspect() {
  local format=$1
  local container=$2
  docker inspect -f "$format" "$container" 2>/dev/null
}

# Reached through the EXIT trap's restoration path.
# shellcheck disable=SC2329
remove_candidates() {
  local -a ids=()
  local id
  while IFS= read -r id; do
    [[ -n $id ]] && ids+=("$id")
  done < <(docker ps -aq --filter "name=^/$CANDIDATE_PREFIX" || true)
  if ((${#ids[@]})); then
    docker rm --force "${ids[@]}" >/dev/null 2>&1 || true
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
  remove_candidates
  if [[ $(inspect '{{.State.Running}}' "$PRODUCTION_CONTAINER") != true ]]; then
    "$PRODUCTION_CONTROLLER" restart || rc=1
  fi
  wait_for_health "$PRODUCTION_PORT" || rc=1
  verify_production_identity || rc=1
  [[ $(inspect '{{.State.Running}}' "$PRODUCTION_CONTAINER") == true ]] || rc=1
  verify_rollback_identity || rc=1
  "$PRODUCTION_CONTROLLER" status > "$PROFILE_RESULT_DIR/production-final-status.txt" 2>&1 || rc=1
  docker inspect -f 'id={{.Id}} image={{.Config.Image}} running={{.State.Running}} restart={{.HostConfig.RestartPolicy.Name}} ports={{json .HostConfig.PortBindings}}' \
    "$PRODUCTION_CONTAINER" > "$PROFILE_RESULT_DIR/production-final-inspect.txt" 2>&1 || rc=1
  docker inspect -f 'id={{.Id}} image={{.Image}} running={{.State.Running}} restart={{.HostConfig.RestartPolicy.Name}} ports={{json .HostConfig.PortBindings}}' \
    "$ROLLBACK_CONTAINER" > "$PROFILE_RESULT_DIR/rollback-final-inspect.txt" 2>&1 || rc=1
  date -u +%Y-%m-%dT%H:%M:%SZ > "$PROFILE_RESULT_DIR/production-restored-at.txt"
  set -e
  return "$rc"
}

# Registered below as the EXIT trap.
# shellcheck disable=SC2329
on_exit() {
  local code=$?
  trap - EXIT
  trap '' HUP INT TERM
  if ! restore_production; then
    code=90
  fi
  printf '%s\n' "$code" > "$PROFILE_RESULT_DIR/exit-status.txt"
  exit "$code"
}

payload_pid=

# A foreground Bash command defers trapped signals until that command returns. Run the payload in
# its own process group, then terminate and reap that whole group before restoring production.
# shellcheck disable=SC2329
terminate_payload_and_exit() {
  local signal=$1
  local code=$2
  local killer_pid=
  trap '' HUP INT TERM
  printf '%s\t%s\n' "$signal" "$code" > "$PROFILE_RESULT_DIR/payload-signal.txt"
  if [[ -n $payload_pid ]] && kill -0 "$payload_pid" 2>/dev/null; then
    kill -s "$signal" -- "-$payload_pid" 2>/dev/null || kill -s "$signal" "$payload_pid" 2>/dev/null || true
    (
      sleep "$payload_stop_timeout"
      kill -KILL -- "-$payload_pid" 2>/dev/null || kill -KILL "$payload_pid" 2>/dev/null || true
    ) &
    killer_pid=$!
    wait "$payload_pid" 2>/dev/null || true
    kill "$killer_pid" 2>/dev/null || true
    wait "$killer_pid" 2>/dev/null || true
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
if docker ps -aq --filter "name=^/$CANDIDATE_PREFIX" | grep -q .; then
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

trap on_exit EXIT
trap 'terminate_payload_and_exit HUP 129' HUP
trap 'terminate_payload_and_exit INT 130' INT
trap 'terminate_payload_and_exit TERM 143' TERM
date -u +%Y-%m-%dT%H:%M:%SZ > "$PROFILE_RESULT_DIR/production-stop-requested-at.txt"
docker stop --time 30 "$PRODUCTION_CONTAINER" >/dev/null
date -u +%Y-%m-%dT%H:%M:%SZ > "$PROFILE_RESULT_DIR/production-stopped-at.txt"
[[ $(inspect '{{.State.Running}}' "$PRODUCTION_CONTAINER") == false ]] || {
  echo "production stop failed" >&2
  exit 3
}
nvidia-smi --query-gpu=name,temperature.gpu,pstate,memory.used,memory.total --format=csv,noheader \
  > "$PROFILE_RESULT_DIR/gpu-after-stop.txt"

set +e
setsid -- "$@" &
payload_pid=$!
wait "$payload_pid"
payload_status=$?
set -e
date -u +%Y-%m-%dT%H:%M:%SZ > "$PROFILE_RESULT_DIR/gpu-work-completed-at.txt"
exit "$payload_status"
