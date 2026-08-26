#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
  echo "usage: $0 CONFIG VARIANT RUN_LABEL" >&2
  exit 2
}

[[ $# -eq 3 ]] || usage
config=$1
variant=$2
run_label=$3
[[ -f "$config" ]] || { echo "profile config not found: $config" >&2; exit 2; }
set -a
# The configuration is a trusted, operator-owned shell environment file.
# shellcheck source=/dev/null
source "$config"
set +a

require_var() {
  local name=$1
  [[ -n ${!name:-} ]] || { echo "profile config is missing $name" >&2; exit 2; }
}

for name in \
  PROFILE_SOURCE_ROOT PROFILE_ARTIFACT_ROOT PROFILE_RESULT_DIR TOOLCHAIN_IMAGE \
  CANDIDATE_PREFIX CANDIDATE_PORT MODEL_PATH MODEL_SHA256 MODEL_ID \
  PROFILE_CONTROLLER_ID PROFILE_TARGET_ID PROFILE_DEPLOYMENT_PREFIX \
  PROFILE_MAX_CONTEXT PROFILE_PREFILL_CHUNK PROFILE_KV_DTYPE PROFILE_MAX_CONCURRENCY \
  PROFILE_DRAFT_TOKENS PROFILE_SEED PROFILE_WARMUP_MESSAGES PROFILE_LONG_MESSAGES \
  PROFILE_DECODE_MESSAGES PROFILE_WARMUP_EXPECTED PROFILE_LONG_EXPECTED; do
  require_var "$name"
done

[[ $variant =~ ^[a-zA-Z0-9][a-zA-Z0-9_.-]*$ ]] || { echo "invalid variant: $variant" >&2; exit 2; }
[[ $run_label =~ ^[a-zA-Z0-9][a-zA-Z0-9_.-]*$ ]] || { echo "invalid run label: $run_label" >&2; exit 2; }
name="$CANDIDATE_PREFIX$run_label"
((${#name} <= 63)) || { echo "candidate container name is longer than 63 bytes" >&2; exit 2; }

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
client="$script_dir/run_profile_request.py"
artifact_dir="$PROFILE_ARTIFACT_ROOT/$variant"
server="$artifact_dir/ninfer-serve"
logdir="$PROFILE_RESULT_DIR/runs/$run_label-$variant"
request_log="$logdir/request.jsonl"
startup_timeout=${PROFILE_STARTUP_TIMEOUT_SECONDS:-600}

[[ -x $server ]] || { echo "candidate server is not executable: $server" >&2; exit 2; }
[[ -f $client ]] || { echo "profile request runner is missing: $client" >&2; exit 2; }
[[ -f $MODEL_PATH ]] || { echo "model artifact is missing: $MODEL_PATH" >&2; exit 2; }
for fixture in "$PROFILE_WARMUP_MESSAGES" "$PROFILE_LONG_MESSAGES" "$PROFILE_DECODE_MESSAGES"; do
  [[ -f "$PROFILE_SOURCE_ROOT/$fixture" ]] || { echo "fixture is missing: $fixture" >&2; exit 2; }
done
actual_model_sha=$(sha256sum "$MODEL_PATH" | cut -d' ' -f1)
[[ $actual_model_sha == "$MODEL_SHA256" ]] || { echo "model artifact SHA-256 mismatch" >&2; exit 2; }
binary_sha=$(sha256sum "$server" | cut -d' ' -f1)

if docker inspect "$name" >/dev/null 2>&1; then
  echo "candidate container already exists: $name" >&2
  exit 2
fi
[[ ! -e $logdir ]] || { echo "cycle output already exists: $logdir" >&2; exit 2; }
mkdir -p "$logdir"
: > "$request_log"

docker run -d --name "$name" --restart no --gpus 'device=0' \
  --publish "127.0.0.1:$CANDIDATE_PORT:8080" \
  -e "NINFER_CONTROLLER_ID=$PROFILE_CONTROLLER_ID" \
  -e "NINFER_TARGET_ID=$PROFILE_TARGET_ID" \
  -v "$artifact_dir:/work:ro" \
  -v "$MODEL_PATH:/models/model.ninfer:ro" \
  -v "$logdir:/logs" \
  --entrypoint /work/ninfer-serve \
  "$TOOLCHAIN_IMAGE" \
  /models/model.ninfer \
  --host 0.0.0.0 --port 8080 --model-id "$MODEL_ID" \
  --binary-sha256 "$binary_sha" \
  --artifact-sha256 "$MODEL_SHA256" \
  --deployment-profile "$PROFILE_DEPLOYMENT_PREFIX-$variant-$run_label" \
  --request-log-jsonl /logs/request.jsonl \
  --log-stats-interval-ms 0 \
  --max-context "$PROFILE_MAX_CONTEXT" --kv-capacity auto \
  --prefill-chunk "$PROFILE_PREFILL_CHUNK" \
  --kv-dtype "$PROFILE_KV_DTYPE" --max-concurrency "$PROFILE_MAX_CONCURRENCY" \
  --spec mtp --draft-tokens "$PROFILE_DRAFT_TOKENS" --lm-head-draft \
  --vision --preserve-thinking \
  > "$logdir/container-id.txt"

cleanup() {
  docker rm --force "$name" >/dev/null 2>&1 || true
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

for i in $(seq 1 "$startup_timeout"); do
  if curl -fsS --max-time 2 "http://127.0.0.1:$CANDIDATE_PORT/health" >/dev/null 2>&1; then
    break
  fi
  if [[ $i -eq $startup_timeout ]]; then
    docker logs --tail 120 "$name" >&2
    exit 1
  fi
  sleep 1
done

python3 - "$request_log" > "$logdir/server-start.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    starts = [json.loads(line) for line in stream if line.strip() and '"event":"server_start"' in line]
if not starts:
    raise SystemExit("server_start event not found")
event = starts[-1]
print(json.dumps({"engine": event.get("engine"), "identity": event.get("identity"), "memory": event.get("memory")}, sort_keys=True))
PY

nvidia-smi --query-gpu=temperature.gpu,clocks.sm,clocks.mem,pstate --format=csv,noheader,nounits \
  > "$logdir/thermal-before.txt"

python3 "$client" --port "$CANDIDATE_PORT" --model "$MODEL_ID" \
  --messages "$PROFILE_SOURCE_ROOT/$PROFILE_WARMUP_MESSAGES" \
  --max-new 8 --seed "$PROFILE_SEED" --expected-content "$PROFILE_WARMUP_EXPECTED" \
  --request-log "$request_log" --output "$logdir/warmup.json"

python3 "$client" --port "$CANDIDATE_PORT" --model "$MODEL_ID" \
  --messages "$PROFILE_SOURCE_ROOT/$PROFILE_LONG_MESSAGES" \
  --max-new 128 --seed "$PROFILE_SEED" --expected-content "$PROFILE_LONG_EXPECTED" \
  --request-log "$request_log" --output "$logdir/long-prefill.json"

nvidia-smi --query-gpu=temperature.gpu,clocks.sm,clocks.mem,pstate --format=csv,noheader,nounits \
  > "$logdir/thermal-after-long.txt"

python3 "$client" --port "$CANDIDATE_PORT" --model "$MODEL_ID" \
  --messages "$PROFILE_SOURCE_ROOT/$PROFILE_DECODE_MESSAGES" \
  --max-new 65536 --seed "$PROFILE_SEED" --thinking \
  --request-log "$request_log" --output "$logdir/mtp3-decode.json"

nvidia-smi --query-gpu=temperature.gpu,clocks.sm,clocks.mem,pstate --format=csv,noheader,nounits \
  > "$logdir/thermal-after-mtp.txt"

docker stop --time 30 "$name" >/dev/null
docker rm "$name" >/dev/null
trap - EXIT HUP INT TERM
