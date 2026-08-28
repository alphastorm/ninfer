#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
  echo "usage: $0 SOURCE_ROOT BUILD_DIR OUTPUT_DIR" >&2
  exit 2
}

[[ $# -eq 3 ]] || usage
source_root=$1
build_dir=$2
output_dir=$3

: "${NINFER_CONTROLLER_ID:?set NINFER_CONTROLLER_ID before profiling}"
: "${NINFER_TARGET_ID:?set NINFER_TARGET_ID before profiling}"
if [[ $NINFER_CONTROLLER_ID == "$NINFER_TARGET_ID" ]]; then
  echo "controller and target identities must differ" >&2
  exit 2
fi

source_root=$(cd "$source_root" && pwd -P)
build_dir=$(cd "$build_dir" && pwd -P)
if [[ -e $output_dir ]]; then
  echo "output directory already exists: $output_dir" >&2
  exit 2
fi

python=${PYTHON:-python3.11}
for command in cmake cuobjdump git ncu nvidia-smi nvcc sha256sum "$python"; do
  command -v "$command" >/dev/null 2>&1 || {
    echo "required profiling command is unavailable: $command" >&2
    exit 2
  }
done

bench="$build_dir/bench/ninfer_q4_linear_swiglu_bench"
[[ -x $bench ]] || {
  echo "Q4 LinearSwiGLU benchmark is not executable: $bench" >&2
  exit 2
}
runner=$(cd "$(dirname "$0")" && pwd -P)/$(basename "$0")
summarizer="$source_root/tools/bench/summarize_ncu_q4.py"
[[ -f $summarizer ]] || {
  echo "Q4 profile summarizer is missing: $summarizer" >&2
  exit 2
}
cache="$build_dir/CMakeCache.txt"
[[ -f $cache ]] || {
  echo "CMake cache is missing: $cache" >&2
  exit 2
}
if ! grep -Fqx "CMAKE_HOME_DIRECTORY:INTERNAL=$source_root" "$cache"; then
  echo "build directory is not configured from $source_root" >&2
  exit 2
fi

source_sha=$(git -C "$source_root" rev-parse HEAD)
source_status=$(git -C "$source_root" status --short)
benchmark_sha=$(sha256sum "$bench" | cut -d' ' -f1)
runner_sha=$(sha256sum "$runner" | cut -d' ' -f1)
summarizer_sha=$(sha256sum "$summarizer" | cut -d' ' -f1)

mkdir -p "$output_dir"
output_dir=$(cd "$output_dir" && pwd -P)

run_logged() {
  local log=$1
  shift
  {
    printf '+ '
    printf '%q ' "$@"
    printf '\n'
    "$@"
  } >"$log" 2>&1
}

{
  printf 'source_sha\t%s\n' "$source_sha"
  printf 'source_status\n'
  printf '%s\n' "$source_status"
  printf 'benchmark_sha256\t%s\n' "$benchmark_sha"
  printf 'runner_sha256\t%s\n' "$runner_sha"
  printf 'summarizer_sha256\t%s\n' "$summarizer_sha"
  printf 'controller_id\t%s\n' "$NINFER_CONTROLLER_ID"
  printf 'target_id\t%s\n' "$NINFER_TARGET_ID"
} >"$output_dir/identity.tsv"

git -C "$source_root" diff --binary HEAD >"$output_dir/source.patch"
cp "$bench" "$output_dir/ninfer_q4_linear_swiglu_bench"
cp "$runner" "$output_dir/profile-runner.sh"
cp "$summarizer" "$output_dir/profile-summarizer.py"
(
  cd "$output_dir"
  sha256sum ninfer_q4_linear_swiglu_bench profile-runner.sh profile-summarizer.py
) >"$output_dir/artifacts.sha256"

cmake -N -LA "$build_dir" >"$output_dir/cmake-cache.txt" 2>&1
nvcc --version >"$output_dir/nvcc.txt" 2>&1
nvidia-smi --query-gpu=name,uuid,driver_version,pstate,power.draw,power.limit,clocks.current.sm,clocks.current.memory,temperature.gpu \
  --format=csv >"$output_dir/gpu-before.csv"
cuobjdump --list-elf "$bench" >"$output_dir/cubins.txt" 2>&1
if ! grep -Eq '(^|[^[:alnum:]_])sm_120a([^[:alnum:]_]|$)' "$output_dir/cubins.txt"; then
  echo "benchmark does not contain a native sm_120a cubin" >&2
  exit 1
fi
cuobjdump --dump-resource-usage "$bench" >"$output_dir/benchmark-resources.txt" 2>&1
cuobjdump --dump-sass "$bench" >"$output_dir/benchmark.sass" 2>&1

"$bench" --describe --route public --t-sweep 1,4 >"$output_dir/shape-manifest.jsonl"
run_logged "$output_dir/timing-public.log" \
  "$bench" --route public --t-sweep 1,4 --warmup 5 --repeat 30
run_logged "$output_dir/timing-materialized.log" \
  "$bench" --route materialized --t-sweep 1,4 --warmup 5 --repeat 30

section_inventory=$(ncu --list-sections 2>&1)
printf '%s\n' "$section_inventory" >"$output_dir/ncu-sections.txt"
section_args=()
for section in LaunchStats Occupancy SpeedOfLight MemoryWorkloadAnalysis SchedulerStats WarpStateStats SourceCounters; do
  if grep -Eq "(^|[[:space:]])${section}([[:space:]]|$)" <<<"$section_inventory"; then
    section_args+=(--section "$section")
  else
    printf 'SKIP unavailable NCU section %s\n' "$section" >>"$output_dir/ncu-sections.txt"
  fi
done
for required in LaunchStats SpeedOfLight MemoryWorkloadAnalysis; do
  if [[ " ${section_args[*]} " != *" $required "* ]]; then
    echo "required NCU section is unavailable: $required" >&2
    exit 1
  fi
done

capture() {
  local m=$1
  local kernel_regex=$2
  local base="$output_dir/q4-m${m}"
  ncu \
    --target-processes all \
    --kernel-name-base function \
    --kernel-name "$kernel_regex" \
    "${section_args[@]}" \
    --launch-count 1 \
    --force-overwrite \
    --export "$base" \
    "$bench" --route public --t-sweep "$m" --profile \
    >"$base.log" 2>&1
  ncu --import "$base.ncu-rep" --csv --page raw >"$base.csv" 2>"$base-export.log"
}

capture 1 'regex:.*q4_linear_swiglu_gemv_pair_kernel.*'
capture 4 'regex:.*q4_small_t_mma_kernel.*'

"$python" "$source_root/tools/bench/summarize_ncu_q4.py" \
  --m1-csv "$output_dir/q4-m1.csv" \
  --m4-csv "$output_dir/q4-m4.csv" \
  --shape-manifest "$output_dir/shape-manifest.jsonl" \
  --output "$output_dir/summary.json"

nvidia-smi --query-gpu=name,uuid,driver_version,pstate,power.draw,power.limit,clocks.current.sm,clocks.current.memory,temperature.gpu \
  --format=csv >"$output_dir/gpu-after.csv"
printf 'profile packet: %s\n' "$output_dir"
