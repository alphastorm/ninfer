#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
  cat >&2 <<EOF
usage: $0 prepare SOURCE_ROOT BUILD_DIR ARTIFACT PREPARE_DIR
       $0 capture SOURCE_ROOT BUILD_DIR ARTIFACT PREPARE_DIR OUTPUT_DIR
EOF
  exit 2
}

[[ $# -ge 1 ]] || usage
mode=$1
shift
case "$mode" in
  prepare) [[ $# -eq 4 ]] || usage ;;
  capture) [[ $# -eq 5 ]] || usage ;;
  *) usage ;;
esac

source_root=$1
build_dir=$2
artifact=$3
prepare_dir=$4
output_dir=${5:-}

: "${NINFER_CONTROLLER_ID:?set NINFER_CONTROLLER_ID before profiling}"
: "${NINFER_TARGET_ID:?set NINFER_TARGET_ID before profiling}"
: "${NINFER_EXPECTED_SOURCE_SHA:?set NINFER_EXPECTED_SOURCE_SHA before profiling}"
: "${NINFER_ARTIFACT_EXPECTED_SHA256:?set NINFER_ARTIFACT_EXPECTED_SHA256 before profiling}"
: "${NINFER_TOOLCHAIN_IMAGE_ID:?set NINFER_TOOLCHAIN_IMAGE_ID before profiling}"
: "${NINFER_UPSTREAM_BASE_SHA:?set NINFER_UPSTREAM_BASE_SHA before profiling}"
if [[ $NINFER_CONTROLLER_ID == "$NINFER_TARGET_ID" ]]; then
  echo "controller and target identities must differ" >&2
  exit 2
fi
[[ $NINFER_EXPECTED_SOURCE_SHA =~ ^[0-9a-f]{40,64}$ ]] || {
  echo "NINFER_EXPECTED_SOURCE_SHA must be a full lowercase Git SHA" >&2
  exit 2
}
[[ $NINFER_UPSTREAM_BASE_SHA =~ ^[0-9a-f]{40,64}$ ]] || {
  echo "NINFER_UPSTREAM_BASE_SHA must be a full lowercase Git SHA" >&2
  exit 2
}
[[ $NINFER_ARTIFACT_EXPECTED_SHA256 =~ ^[0-9a-f]{64}$ ]] || {
  echo "NINFER_ARTIFACT_EXPECTED_SHA256 must be a lowercase SHA-256" >&2
  exit 2
}
[[ $NINFER_TOOLCHAIN_IMAGE_ID =~ ^sha256:[0-9a-f]{64}$ ]] || {
  echo "NINFER_TOOLCHAIN_IMAGE_ID must be an exact sha256 image ID" >&2
  exit 2
}

source_root=$(cd "$source_root" && pwd -P)
build_dir=$(cd "$build_dir" && pwd -P)
artifact=$(cd "$(dirname "$artifact")" && pwd -P)/$(basename "$artifact")
prepare_dir=$(cd "$prepare_dir" && pwd -P)
[[ -f $artifact ]] || { echo "artifact is not a regular file: $artifact" >&2; exit 2; }

python=${PYTHON:-python3.11}
for command in git sha256sum "$python"; do
  command -v "$command" >/dev/null 2>&1 || {
    echo "required profiling command is unavailable: $command" >&2
    exit 2
  }
done
python_version=$("$python" -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')
[[ $python_version == 3.11 ]] || {
  echo "profiling requires the selected Python 3.11 interpreter" >&2
  exit 2
}

source_sha=$(git -C "$source_root" rev-parse HEAD)
[[ $source_sha == "$NINFER_EXPECTED_SOURCE_SHA" ]] || {
  echo "source HEAD differs from NINFER_EXPECTED_SOURCE_SHA" >&2
  exit 2
}
source_status=$(git -C "$source_root" status --porcelain --untracked-files=all)
[[ -z $source_status ]] || {
  echo "the fixed profiling packet requires a completely clean committed worktree" >&2
  exit 2
}
git -C "$source_root" cat-file -e "$NINFER_UPSTREAM_BASE_SHA^{commit}" 2>/dev/null || {
  echo "NINFER_UPSTREAM_BASE_SHA is not a commit in SOURCE_ROOT" >&2
  exit 2
}
git -C "$source_root" merge-base --is-ancestor "$NINFER_UPSTREAM_BASE_SHA" "$source_sha" || {
  echo "NINFER_UPSTREAM_BASE_SHA is not an ancestor of NINFER_EXPECTED_SOURCE_SHA" >&2
  exit 2
}
artifact_sha=$(sha256sum "$artifact" | cut -d' ' -f1)
[[ $artifact_sha == "$NINFER_ARTIFACT_EXPECTED_SHA256" ]] || {
  echo "artifact SHA-256 differs from NINFER_ARTIFACT_EXPECTED_SHA256" >&2
  exit 2
}

q4_runner="$source_root/tools/bench/run_sm120_q4_mtp3_profile.sh"
profile_runner="$source_root/tools/bench/run_sm120_mtp3_profile.sh"
workflow_runner="$source_root/tools/bench/run_sm120_mtp3_workflow.sh"
q4_summarizer="$source_root/tools/bench/summarize_ncu_q4.py"
for script in "$q4_runner" "$profile_runner" "$workflow_runner"; do
  [[ -x $script ]] || { echo "profiling runner is not executable: $script" >&2; exit 2; }
done
[[ -f $q4_summarizer ]] || { echo "Q4 profile summarizer is missing: $q4_summarizer" >&2; exit 2; }

q4_bench="$build_dir/bench/ninfer_q4_linear_swiglu_bench"
q5_bench="$build_dir/bench/ninfer_q5_linear_add_bench"
post_mixer_bench="$build_dir/bench/ninfer_qwen3_6_27b_post_mixer_bench"
mtp_round_bench="$build_dir/bench/ninfer_qwen3_6_27b_mtp_round_bench"
q4_test="$build_dir/tests/ninfer_linear_swiglu_q4_a16_test"
q5_test="$build_dir/tests/ninfer_linear_add_q5_a16_test"
profile_binaries=(
  "$q4_bench"
  "$q5_bench"
  "$post_mixer_bench"
  "$mtp_round_bench"
  "$q4_test"
  "$q5_test"
)

run_logged() {
  local log=$1
  shift
  {
    printf 'command:'
    printf ' %q' "$@"
    printf '\n'
    "$@"
  } >"$log" 2>&1
}

verify_native_sm120a() {
  local label=$1
  local binary=$2
  local inventory="$prepare_dir/environment/${label}.cubins.txt"
  cuobjdump --list-elf "$binary" >"$inventory" 2>&1
  if ! grep -Eq '(^|[^[:alnum:]_])sm_120a([^[:alnum:]_]|$)' "$inventory"; then
    echo "$label does not contain a native sm_120a cubin" >&2
    exit 1
  fi
  cuobjdump --dump-resource-usage "$binary" \
    >"$prepare_dir/environment/${label}.resources.txt" 2>&1
}

if [[ $mode == prepare ]]; then
  for command in cmake cuobjdump ncu nsys nvcc timeout; do
    command -v "$command" >/dev/null 2>&1 || {
      echo "required preparation command is unavailable: $command" >&2
      exit 2
    }
  done
  shopt -s nullglob dotglob
  build_entries=("$build_dir"/*)
  prepare_entries=("$prepare_dir"/*)
  shopt -u nullglob dotglob
  ((${#build_entries[@]} == 0)) || { echo "build directory must be empty" >&2; exit 2; }
  ((${#prepare_entries[@]} == 0)) || { echo "prepare directory must be empty" >&2; exit 2; }

  mkdir -p "$prepare_dir/environment"
  run_logged "$prepare_dir/environment/configure.log" \
    cmake -S "$source_root" -B "$build_dir" -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_CUDA_ARCHITECTURES=120a \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      -DNINFER_BUILD_APPS=ON \
      -DBUILD_TESTING=ON \
      -DNINFER_BUILD_BENCHMARKS=ON \
      -DNINFER_BUILD_PROFILE=profile \
      -DNINFER_UPSTREAM_BASE_SHA="$NINFER_UPSTREAM_BASE_SHA" \
      -DNINFER_PATCH_STACK_SHA="$NINFER_EXPECTED_SOURCE_SHA" \
      -DNINFER_SOURCE_CLEAN_VERIFIED=ON
  run_logged "$prepare_dir/environment/build.log" \
    cmake --build "$build_dir" -j --target \
      ninfer_q4_linear_swiglu_bench \
      ninfer_q5_linear_add_bench \
      ninfer_qwen3_6_27b_post_mixer_bench \
      ninfer_qwen3_6_27b_mtp_round_bench \
      ninfer_linear_swiglu_q4_a16_test \
      ninfer_linear_add_q5_a16_test

  for binary in "${profile_binaries[@]}"; do
    [[ -x $binary ]] || { echo "required profiling binary is not executable: $binary" >&2; exit 2; }
  done
  verify_native_sm120a q4 "$q4_bench"
  verify_native_sm120a q5 "$q5_bench"
  verify_native_sm120a post-mixer "$post_mixer_bench"
  verify_native_sm120a mtp-round "$mtp_round_bench"

  (
    sha256sum \
      "${profile_binaries[@]}" \
      "$q4_runner" \
      "$profile_runner" \
      "$workflow_runner" \
      "$q4_summarizer"
  ) >"$prepare_dir/artifacts.sha256"
  sha256sum "$artifact" >"$prepare_dir/artifact.sha256"
  cmake -N -LA "$build_dir" >"$prepare_dir/environment/cmake-cache.txt" 2>&1
  nvcc --version >"$prepare_dir/environment/nvcc.txt" 2>&1
  "$python" --version >"$prepare_dir/environment/python.txt" 2>&1

  "$python" - "$prepare_dir/manifest.json" "$source_root" "$build_dir" "$artifact" \
    "$source_sha" "$artifact_sha" "$NINFER_CONTROLLER_ID" "$NINFER_TARGET_ID" \
    "$NINFER_TOOLCHAIN_IMAGE_ID" "$NINFER_UPSTREAM_BASE_SHA" <<'PY'
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

(
    output,
    source,
    build,
    artifact,
    source_sha,
    artifact_sha,
    controller,
    target,
    toolchain_image_id,
    upstream_base_sha,
) = sys.argv[1:]
manifest = {
    "schema_version": 1,
    "campaign": "sm120-mtp3-prepare",
    "created_at": datetime.now(timezone.utc).isoformat(),
    "source_root": source,
    "build_dir": build,
    "source_sha": source_sha,
    "source_clean": True,
    "upstream_base_sha": upstream_base_sha,
    "artifact": artifact,
    "artifact_sha256": artifact_sha,
    "controller_id": controller,
    "target_id": target,
    "toolchain_image_id": toolchain_image_id,
}
Path(output).write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
PY
  date -u +%Y-%m-%dT%H:%M:%SZ >"$prepare_dir/prepare-complete.txt"
  printf 'profile preparation: %s\n' "$prepare_dir"
  exit 0
fi

for command in cmake cuobjdump ncu nsys nvidia-smi nvcc timeout; do
  command -v "$command" >/dev/null 2>&1 || {
    echo "required capture command is unavailable: $command" >&2
    exit 2
  }
done
cache="$build_dir/CMakeCache.txt"
[[ -f $cache ]] || { echo "CMake cache is missing: $cache" >&2; exit 2; }
if ! grep -Fqx "CMAKE_HOME_DIRECTORY:INTERNAL=$source_root" "$cache"; then
  echo "build directory is not configured from $source_root" >&2
  exit 2
fi
for binary in "${profile_binaries[@]}"; do
  [[ -x $binary ]] || { echo "required profiling binary is not executable: $binary" >&2; exit 2; }
done
[[ -f $prepare_dir/prepare-complete.txt && -f $prepare_dir/manifest.json &&
   -f $prepare_dir/artifacts.sha256 ]] || {
  echo "profiling preparation is incomplete" >&2
  exit 2
}
artifacts_check=$(mktemp)
trap 'rm -f -- "$artifacts_check"' EXIT
sha256sum --check --strict "$prepare_dir/artifacts.sha256" >"$artifacts_check"
"$python" - "$prepare_dir/manifest.json" "$source_sha" "$artifact_sha" \
  "$NINFER_TOOLCHAIN_IMAGE_ID" "$NINFER_UPSTREAM_BASE_SHA" <<'PY'
import json
import sys
from pathlib import Path

manifest = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
expected = {
    "schema_version": 1,
    "campaign": "sm120-mtp3-prepare",
    "source_sha": sys.argv[2],
    "source_clean": True,
    "artifact_sha256": sys.argv[3],
    "toolchain_image_id": sys.argv[4],
    "upstream_base_sha": sys.argv[5],
}
for key, value in expected.items():
    if manifest.get(key) != value:
        raise SystemExit(f"preparation manifest mismatch: {key}")
PY

[[ ! -e $output_dir ]] || { echo "output directory already exists: $output_dir" >&2; exit 2; }
mkdir -p "$output_dir"/{environment,correctness,nsys,ncu,timings,preparation}
output_dir=$(cd "$output_dir" && pwd -P)
cp -a "$prepare_dir"/. "$output_dir/preparation/"
cp "$artifacts_check" "$output_dir/preparation/artifacts-check.log"

ncu_timeout=${NINFER_NCU_TIMEOUT_SECONDS:-180}
nsys_timeout=${NINFER_NSYS_TIMEOUT_SECONDS:-300}
for timeout_name in ncu_timeout nsys_timeout; do
  timeout_value=${!timeout_name}
  if ! [[ $timeout_value =~ ^[0-9]+$ ]] || ((timeout_value < 1)); then
    echo "$timeout_name must be a positive integer" >&2
    exit 2
  fi
done

{
  printf 'source_sha\t%s\n' "$source_sha"
  printf 'source_clean\ttrue\n'
  printf 'artifact\t%s\n' "$artifact"
  printf 'artifact_sha256\t%s\n' "$artifact_sha"
  printf 'controller_id\t%s\n' "$NINFER_CONTROLLER_ID"
  printf 'target_id\t%s\n' "$NINFER_TARGET_ID"
  printf 'toolchain_image_id\t%s\n' "$NINFER_TOOLCHAIN_IMAGE_ID"
} >"$output_dir/identity.tsv"
sha256sum "$artifact" >"$output_dir/artifact.sha256"
cmake -N -LA "$build_dir" >"$output_dir/environment/cmake-cache.txt" 2>&1
nvcc --version >"$output_dir/environment/nvcc.txt" 2>&1
ncu --version >"$output_dir/environment/ncu.txt" 2>&1
nsys --version >"$output_dir/environment/nsys.txt" 2>&1
nvidia-smi --query-gpu=name,uuid,driver_version,pstate,power.draw,power.limit,clocks.current.sm,clocks.current.memory,temperature.gpu \
  --format=csv >"$output_dir/environment/gpu-before.csv"

"$python" - "$output_dir/manifest.json" "$source_root" "$build_dir" "$artifact" \
  "$source_sha" "$artifact_sha" "$NINFER_CONTROLLER_ID" "$NINFER_TARGET_ID" \
  "$NINFER_TOOLCHAIN_IMAGE_ID" <<'PY'
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

output, source, build, artifact, source_sha, artifact_sha, controller, target, toolchain = sys.argv[1:]
manifest = {
    "schema_version": 2,
    "campaign": "sm120-mtp3-attribution",
    "created_at": datetime.now(timezone.utc).isoformat(),
    "source_root": source,
    "build_dir": build,
    "source_sha": source_sha,
    "source_clean": True,
    "artifact": artifact,
    "artifact_sha256": artifact_sha,
    "controller_id": controller,
    "target_id": target,
    "toolchain_image_id": toolchain,
    "counter_access_gate": "q5-split2-cold",
    "target_verify_width": 4,
    "profiles": [
        "mtp3-round-nsys",
        "q4-m1-ncu",
        "q4-m4-ncu",
        "q5-split2-cold-ncu",
        "q5-split2-q4-produced-ncu",
        "q5-c16-q4-produced-ncu",
    ],
}
Path(output).write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
PY

run_logged "$output_dir/correctness/q4-t4.log" "$q4_test"
run_logged "$output_dir/correctness/q5-public-and-c16-t4.log" "$q5_test"

section_inventory=$(timeout "$ncu_timeout" ncu --list-sections 2>&1)
printf '%s\n' "$section_inventory" >"$output_dir/ncu/sections.txt"
section_args=()
for section in LaunchStats Occupancy SpeedOfLight MemoryWorkloadAnalysis SchedulerStats WarpStateStats SourceCounters; do
  if grep -Eq "(^|[[:space:]])${section}([[:space:]]|$)" <<<"$section_inventory"; then
    section_args+=(--section "$section")
  else
    printf 'SKIP unavailable NCU section %s\n' "$section" >>"$output_dir/ncu/sections.txt"
  fi
done
for required in LaunchStats SpeedOfLight MemoryWorkloadAnalysis; do
  if [[ " ${section_args[*]} " != *" $required "* ]]; then
    echo "required NCU section is unavailable: $required" >&2
    exit 1
  fi
done

capture_q5() {
  local name=$1
  local state=$2
  local candidate=$3
  local base="$output_dir/ncu/$name"
  if ! timeout "$ncu_timeout" ncu \
      --profile-from-start off \
      --target-processes all \
      --kernel-name-base function \
      "${section_args[@]}" \
      --launch-count 1 \
      --force-overwrite \
      --export "$base" \
      "$post_mixer_bench" --activation-state "$state" --candidate "$candidate" \
        --measure q5 --warmup 2 --repeat 1 --profile-q5 \
      >"$base.log" 2>&1; then
    if grep -Fq ERR_NVGPUCTRPERM "$base.log"; then
      echo "NCU performance-counter access is unavailable; keep Docker Desktop WSL integration disabled and restore the configured WSL daemon before retrying" >&2
    else
      echo "NCU capture failed: $name" >&2
    fi
    return 1
  fi
  [[ -s $base.ncu-rep ]] || { echo "NCU emitted no report: $name" >&2; return 1; }
  if ! timeout "$ncu_timeout" ncu --import "$base.ncu-rep" --csv --page raw \
      >"$base.csv" 2>"$base-export.log"; then
    echo "NCU report export failed: $name" >&2
    return 1
  fi
  [[ -s $base.csv ]] || { echo "NCU emitted an empty CSV export: $name" >&2; return 1; }
  if grep -Fq ERR_NVGPUCTRPERM "$base.log" "$base-export.log"; then
    echo "NCU performance-counter access failed: $name" >&2
    return 1
  fi
}

capture_q5 q5-split2-cold cold public
{
  date -u +%Y-%m-%dT%H:%M:%SZ
  sha256sum "$output_dir/ncu/q5-split2-cold.ncu-rep" \
    "$output_dir/ncu/q5-split2-cold.csv"
} >"$output_dir/ncu/counter-access-verified.txt"


"$post_mixer_bench" --describe --activation-state q4-produced --candidate public --measure q5 \
  >"$output_dir/post-mixer-manifest.json"
run_logged "$output_dir/timings/q5-public-cold.log" \
  "$q5_bench" --k 17408 --t-sweep 4 --candidate public --warmup 5 --repeat 30
run_logged "$output_dir/timings/q5-c16-cold.log" \
  "$q5_bench" --k 17408 --t-sweep 4 --candidate mma-r64-c16 --warmup 5 --repeat 30

: >"$output_dir/timings/post-mixer.jsonl"
for state in cold q4-produced hot; do
  "$post_mixer_bench" --activation-state "$state" --candidate public --measure q5 \
    --warmup 5 --repeat 30 >>"$output_dir/timings/post-mixer.jsonl"
done
"$post_mixer_bench" --activation-state q4-produced --candidate mma-r64-c16 --measure q5 \
  --warmup 5 --repeat 30 >>"$output_dir/timings/post-mixer.jsonl"
"$post_mixer_bench" --activation-state q4-produced --candidate public --measure chain \
  --warmup 5 --repeat 30 >>"$output_dir/timings/post-mixer.jsonl"
"$mtp_round_bench" --artifact "$artifact" --draft-tokens 3 --proposal-head optimized \
  --warmup 2 --reps 10 >"$output_dir/timings/mtp-round.csv" \
  2>"$output_dir/timings/mtp-round.stderr.log"

"$python" - "$output_dir/timings/post-mixer.jsonl" \
  "$output_dir/timings/summary.json" <<'PY'
import json
import sys
from pathlib import Path

records = [json.loads(line) for line in Path(sys.argv[1]).read_text(encoding="utf-8").splitlines()]
index = {
    (record["activation_state"], record["candidate"], record["timed_scope"]): record
    for record in records
}
def median(state, candidate="public", scope="q5"):
    return index[(state, candidate, scope)]["median_us"]

cold = median("cold")
produced = median("q4-produced")
hot = median("hot")
c16 = median("q4-produced", "mma-r64-c16")
ratio = c16 / produced
if ratio <= 1.20:
    c16_triage = "within-20-percent"
elif ratio <= 1.50:
    c16_triage = "20-to-50-percent-slower"
else:
    c16_triage = "more-than-50-percent-slower"
summary = {
    "schema_version": 1,
    "historical_split2_reference_us": 59.04,
    "public_split2_us": {"cold": cold, "q4_produced": produced, "hot": hot},
    "ratios": {
        "cold_over_q4_produced": cold / produced,
        "q4_produced_over_hot": produced / hot,
        "c16_over_public_q4_produced": ratio,
    },
    "c16_triage": c16_triage,
    "kernel_decision_requires_nsys_and_ncu_review": True,
}
Path(sys.argv[2]).write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
PY

run_logged "$output_dir/nsys/mtp3-round.log" \
  timeout "$nsys_timeout" nsys profile --trace=cuda,nvtx,osrt --capture-range=cudaProfilerApi \
    --stop-on-range-end=true --force-overwrite=true \
    -o "$output_dir/nsys/mtp3-round" \
    "$mtp_round_bench" --artifact "$artifact" --draft-tokens 3 --proposal-head optimized \
      --warmup 2 --reps 1 --profile-measured
timeout "$nsys_timeout" nsys stats --report cuda_gpu_kern_sum --format csv \
  "$output_dir/nsys/mtp3-round.nsys-rep" >"$output_dir/nsys/summary.csv" \
  2>"$output_dir/nsys/summary.stderr.log"


capture_q5 q5-split2-q4-produced q4-produced public
capture_q5 q5-c16-q4-produced q4-produced mma-r64-c16

NINFER_CONTROLLER_ID=$NINFER_CONTROLLER_ID \
NINFER_TARGET_ID=$NINFER_TARGET_ID \
  timeout "$((ncu_timeout * 4))" "$q4_runner" "$source_root" "$build_dir" "$output_dir/ncu/q4-packet"

nvidia-smi --query-gpu=name,uuid,driver_version,pstate,power.draw,power.limit,clocks.current.sm,clocks.current.memory,temperature.gpu \
  --format=csv >"$output_dir/environment/gpu-after.csv"
date -u +%Y-%m-%dT%H:%M:%SZ >"$output_dir/packet-complete.txt"
printf 'profile packet: %s\n' "$output_dir"
