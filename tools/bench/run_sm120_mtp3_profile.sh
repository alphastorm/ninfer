#!/usr/bin/env bash
set -Eeuo pipefail
usage() { echo "usage: $0 prepare SOURCE_ROOT BUILD_DIR ARTIFACT PREPARE_DIR" >&2; echo "       $0 capture SOURCE_ROOT BUILD_DIR ARTIFACT PREPARE_DIR OUTPUT_DIR" >&2; exit 2; }
[[ $# -ge 1 ]] || usage
mode=$1; shift
case "$mode" in prepare) [[ $# -eq 4 ]] || usage ;; capture) [[ $# -eq 5 ]] || usage ;; *) usage ;; esac
source_root=$(cd "$1" && pwd -P)
build_dir=$(cd "$2" && pwd -P)
artifact=$(cd "$(dirname "$3")" && pwd -P)/$(basename "$3")
prepare_dir=$(cd "$4" && pwd -P)
output_dir=${5:-}
for name in NINFER_CONTROLLER_ID NINFER_TARGET_ID NINFER_EXPECTED_SOURCE_SHA NINFER_ARTIFACT_EXPECTED_SHA256 NINFER_TOOLCHAIN_IMAGE_ID NINFER_UPSTREAM_BASE_SHA; do
  [[ -n ${!name:-} ]] || { echo "set $name before profiling" >&2; exit 2; }
done
[[ $NINFER_CONTROLLER_ID != "$NINFER_TARGET_ID" ]] || { echo 'controller and target identities must differ' >&2; exit 2; }
[[ $NINFER_EXPECTED_SOURCE_SHA =~ ^[0-9a-f]{40}$ && $NINFER_UPSTREAM_BASE_SHA =~ ^[0-9a-f]{40}$ ]] || { echo 'source identities must be full lowercase Git SHAs' >&2; exit 2; }
[[ $NINFER_ARTIFACT_EXPECTED_SHA256 =~ ^[0-9a-f]{64}$ && $NINFER_TOOLCHAIN_IMAGE_ID =~ ^sha256:[0-9a-f]{64}$ ]] || { echo 'artifact and toolchain identities must be SHA-256' >&2; exit 2; }
python=${PYTHON:-python3.11}
for command in git sha256sum "$python" cmake cuobjdump ncu nsys nvcc timeout; do
  command -v "$command" >/dev/null || { echo "required preparation command is unavailable: $command" >&2; exit 2; }
done
[[ $("$python" -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")') == 3.11 ]] || { echo 'profiling requires the selected Python 3.11 interpreter' >&2; exit 2; }
source_sha=$(git -C "$source_root" rev-parse HEAD)
[[ $source_sha == "$NINFER_EXPECTED_SOURCE_SHA" ]] || { echo 'source HEAD differs from NINFER_EXPECTED_SOURCE_SHA' >&2; exit 2; }
[[ -z $(git -C "$source_root" status --porcelain --untracked-files=all) ]] || { echo 'the fixed profiling packet requires a completely clean committed worktree' >&2; exit 2; }
git -C "$source_root" cat-file -e "$NINFER_UPSTREAM_BASE_SHA^{commit}"
git -C "$source_root" merge-base --is-ancestor "$NINFER_UPSTREAM_BASE_SHA" "$source_sha" || { echo 'NINFER_UPSTREAM_BASE_SHA is not an ancestor of NINFER_EXPECTED_SOURCE_SHA' >&2; exit 2; }
[[ -f $artifact ]] || { echo 'artifact is not a regular file' >&2; exit 2; }
artifact_sha=$(sha256sum "$artifact" | cut -d' ' -f1)
[[ $artifact_sha == "$NINFER_ARTIFACT_EXPECTED_SHA256" ]] || { echo 'artifact SHA-256 differs from NINFER_ARTIFACT_EXPECTED_SHA256' >&2; exit 2; }
q4_runner="$source_root/tools/bench/run_sm120_q4_mtp3_profile.sh"
q4_summarizer="$source_root/tools/bench/summarize_ncu_q4.py"
corpus="$source_root/bench/fixtures/bench_corpus.ids"
for script in "$q4_runner" "$source_root/tools/bench/run_sm120_mtp3_workflow.sh"; do
  [[ -x $script ]] || { echo "profiling runner is not executable: $script" >&2; exit 2; }
done
[[ -f $q4_summarizer && -f $corpus ]] || { echo 'profiling summarizer or token corpus is missing' >&2; exit 2; }
q4_bench="$build_dir/bench/ninfer_q4_linear_swiglu_bench"
q5_bench="$build_dir/bench/ninfer_q5_linear_add_bench"
mtp_round_bench="$build_dir/bench/ninfer_qwen3_5_mtp_round_bench"
engine_bench="$build_dir/bench/ninfer_bench"
q4_test="$build_dir/tests/ninfer_linear_swiglu_q4_a16_test"
q5_test="$build_dir/tests/ninfer_linear_add_q5_a16_test"
profile_binaries=("$q4_bench" "$q5_bench" "$mtp_round_bench" "$engine_bench" "$q4_test" "$q5_test")
run_logged() { local log=$1; shift; { printf 'command:'; printf ' %q' "$@"; printf '\n'; "$@"; } >"$log" 2>&1; }
verify_binaries() {
  for binary in "${profile_binaries[@]}"; do
    [[ -x $binary ]] || { echo "required profiling binary is not executable: $binary" >&2; exit 2; }
  done
}
if [[ $mode == prepare ]]; then
  shopt -s nullglob dotglob
  build_entries=("$build_dir"/*); prepare_entries=("$prepare_dir"/*)
  shopt -u nullglob dotglob
  ((${#build_entries[@]} == 0 && ${#prepare_entries[@]} == 0)) || { echo 'build and preparation directories must be empty' >&2; exit 2; }
  mkdir -p "$prepare_dir/environment"
  run_logged "$prepare_dir/environment/nsys-profile-help.log" nsys profile --capture-range=cudaProfilerApi --capture-range-end=stop --cuda-graph-trace=node --help
  run_logged "$prepare_dir/environment/configure.log" cmake -S "$source_root" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CUDA_ARCHITECTURES=120a \
    -DNINFER_BUILD_APPS=ON -DBUILD_TESTING=ON -DNINFER_BUILD_BENCHMARKS=ON \
    -DNINFER_BUILD_PROFILE=profile -DNINFER_UPSTREAM_BASE_SHA="$NINFER_UPSTREAM_BASE_SHA" -DNINFER_PATCH_STACK_SHA="$source_sha"
  run_logged "$prepare_dir/environment/build.log" cmake --build "$build_dir" -j "${NINFER_BUILD_JOBS:-8}" --target \
    ninfer_q4_linear_swiglu_bench ninfer_q5_linear_add_bench ninfer_qwen3_5_mtp_round_bench ninfer_bench \
    ninfer_linear_swiglu_q4_a16_test ninfer_linear_add_q5_a16_test
  verify_binaries
  for binary in "$q4_bench" "$q5_bench" "$mtp_round_bench" "$engine_bench"; do
    label=$(basename "$binary")
    cuobjdump --list-elf "$binary" >"$prepare_dir/environment/$label.cubins.txt" 2>&1
    grep -Eq '(^|[^[:alnum:]_])sm_120a([^[:alnum:]_]|$)' "$prepare_dir/environment/$label.cubins.txt" || { echo "$label does not contain a native sm_120a cubin" >&2; exit 1; }
    cuobjdump --dump-resource-usage "$binary" >"$prepare_dir/environment/$label.resources.txt" 2>&1
  done
  sha256sum "${profile_binaries[@]}" "$q4_runner" "$0" "$q4_summarizer" "$corpus" >"$prepare_dir/artifacts.sha256"
  "$python" - "$prepare_dir/manifest.json" "$source_sha" "$artifact_sha" "$NINFER_TOOLCHAIN_IMAGE_ID" "$NINFER_UPSTREAM_BASE_SHA" <<'PY'
import json
import sys
from pathlib import Path
output, source, artifact, image, base = sys.argv[1:]
Path(output).write_text(json.dumps(dict(schema_version=2, campaign='sm120-mtp3-prepare', source_sha=source, source_clean=True, upstream_base_sha=base, artifact_sha256=artifact, toolchain_image_id=image), sort_keys=True) + '\n', encoding='utf-8')
PY
  date -u +%Y-%m-%dT%H:%M:%SZ >"$prepare_dir/prepare-complete.txt"
  printf 'profile preparation: %s\n' "$prepare_dir"
  exit 0
fi
command -v nvidia-smi >/dev/null || { echo 'required capture command is unavailable: nvidia-smi' >&2; exit 2; }
verify_binaries
grep -Fqx "CMAKE_HOME_DIRECTORY:INTERNAL=$source_root" "$build_dir/CMakeCache.txt" || { echo 'build directory is not configured from SOURCE_ROOT' >&2; exit 2; }
[[ -f $prepare_dir/prepare-complete.txt ]] || { echo 'profiling preparation is incomplete' >&2; exit 2; }
sha256sum --check --strict "$prepare_dir/artifacts.sha256"
"$python" - "$prepare_dir/manifest.json" "$source_sha" "$artifact_sha" "$NINFER_TOOLCHAIN_IMAGE_ID" "$NINFER_UPSTREAM_BASE_SHA" <<'PY'
import json
import sys
from pathlib import Path
value = json.loads(Path(sys.argv[1]).read_text(encoding='utf-8'))
expected = dict(schema_version=2, campaign='sm120-mtp3-prepare', source_sha=sys.argv[2], artifact_sha256=sys.argv[3], toolchain_image_id=sys.argv[4], upstream_base_sha=sys.argv[5], source_clean=True)
if value != expected:
    raise SystemExit('preparation manifest mismatch')
PY
[[ ! -e $output_dir ]] || { echo 'output directory already exists' >&2; exit 2; }
ncu_timeout=${NINFER_NCU_TIMEOUT_SECONDS:-180}
nsys_timeout=${NINFER_NSYS_TIMEOUT_SECONDS:-300}
for value in "$ncu_timeout" "$nsys_timeout"; do [[ $value =~ ^[1-9][0-9]*$ ]] || { echo 'profiler timeouts must be positive integers' >&2; exit 2; }; done
mkdir -p "$output_dir"/{environment,correctness,nsys,ncu,timings,preparation}
output_dir=$(cd "$output_dir" && pwd -P)
cp -a "$prepare_dir"/. "$output_dir/preparation/"
sha256sum --check --strict "$prepare_dir/artifacts.sha256" >"$output_dir/preparation/artifacts-check.log"
nvidia-smi --query-gpu=name,uuid,driver_version,temperature.gpu --format=csv >"$output_dir/environment/gpu-before.csv"
run_logged "$output_dir/correctness/q4.log" "$q4_test"
run_logged "$output_dir/correctness/q5.log" "$q5_test"
sections=$(timeout "$ncu_timeout" ncu --list-sections 2>&1)
printf '%s\n' "$sections" >"$output_dir/ncu/sections.txt"
section_args=()
for section in LaunchStats SpeedOfLight MemoryWorkloadAnalysis; do
  grep -Eq "(^|[[:space:]])${section}([[:space:]]|$)" <<<"$sections" || { echo "required NCU section is unavailable: $section" >&2; exit 1; }
  section_args+=(--section "$section")
done
# The public master benchmark has no candidate/private-route switches and no
# profiler API boundary for its one-call --profile mode. Filter the first Q5 Op.
if ! timeout "$ncu_timeout" ncu --target-processes all --kernel-name-base function \
    --kernel-name 'regex:.*q5.*' "${section_args[@]}" --launch-count 1 --force-overwrite \
    --export "$output_dir/ncu/q5-public" "$q5_bench" --k 17408 --t-sweep 4 --profile \
    >"$output_dir/ncu/q5-public.log" 2>&1; then
  if grep -Fq ERR_NVGPUCTRPERM "$output_dir/ncu/q5-public.log"; then
    echo 'NCU performance-counter access is unavailable' >&2
  else echo 'NCU capture failed: q5-public' >&2; fi
  exit 1
fi
[[ -s $output_dir/ncu/q5-public.ncu-rep ]] || { echo 'NCU emitted no report' >&2; exit 1; }
timeout "$ncu_timeout" ncu --import "$output_dir/ncu/q5-public.ncu-rep" --csv --page raw >"$output_dir/ncu/q5-public.csv" 2>"$output_dir/ncu/q5-public-export.log"
[[ -s $output_dir/ncu/q5-public.csv ]] || { echo 'NCU emitted an empty CSV export' >&2; exit 1; }
date -u +%Y-%m-%dT%H:%M:%SZ >"$output_dir/ncu/counter-access-verified.txt"
"$mtp_round_bench" --artifact "$artifact" --draft-tokens 3 --proposal-head optimized --warmup 2 --reps 10 >"$output_dir/timings/mtp-round.csv" 2>"$output_dir/timings/mtp-round.stderr.log"
for execution in eager graph; do
  run_logged "$output_dir/timings/q5-$execution.log" "$q5_bench" --k 17408 --t-sweep 4 --execution "$execution" --warmup 5 --repeat 30 --csv-out "$output_dir/timings/q5-$execution.csv"
done
run_logged "$output_dir/nsys/mtp3-round.log" timeout "$nsys_timeout" nsys profile --trace=cuda,nvtx,osrt \
  --capture-range=cudaProfilerApi --capture-range-end=stop --cuda-graph-trace=node --force-overwrite=true \
  -o "$output_dir/nsys/mtp3-round" "$engine_bench" --weights "$artifact" --corpus "$corpus" \
  -n 32 -r 1 --warmup 1 --spec mtp --draft-tokens 3 --lm-head-draft --profile-measured
[[ -s $output_dir/nsys/mtp3-round.nsys-rep ]] || { echo 'Nsight Systems emitted no report' >&2; exit 1; }
timeout "$nsys_timeout" nsys stats --report cuda_gpu_kern_sum --format csv "$output_dir/nsys/mtp3-round.nsys-rep" >"$output_dir/nsys/summary.csv" 2>"$output_dir/nsys/summary.stderr.log"
"$python" - "$output_dir/nsys/summary.csv" "$output_dir/nsys/summary-verified.txt" <<'PY'
import csv
import sys
from pathlib import Path
rows = list(csv.reader(Path(sys.argv[1]).read_text(encoding='utf-8').splitlines()))
header = next((i for i, row in enumerate(rows) if 'Total Time (ns)' in row and 'Name' in row), None)
if header is None:
    raise SystemExit('Nsight Systems kernel summary header was not found')
time_index, name_index = rows[header].index('Total Time (ns)'), rows[header].index('Name')
kernels = [r for r in rows[header + 1:] if len(r) == len(rows[header]) and r[name_index]]
if len(kernels) < 2 or sum(float(r[time_index].replace(',', '')) for r in kernels) < 1e6:
    raise SystemExit('Nsight Systems did not expose the CUDA Graph node topology')
Path(sys.argv[2]).write_text(f'kernel_rows\t{len(kernels)}\n', encoding='utf-8')
PY
timeout "$((ncu_timeout * 4))" "$q4_runner" "$source_root" "$build_dir" "$output_dir/ncu/q4-packet"
"$python" - "$output_dir/manifest.json" "$source_sha" "$artifact_sha" "$NINFER_CONTROLLER_ID" "$NINFER_TARGET_ID" "$NINFER_TOOLCHAIN_IMAGE_ID" <<'PY'
import json
import sys
from pathlib import Path
output, source, artifact, controller, target, image = sys.argv[1:]
Path(output).write_text(json.dumps(dict(schema_version=3, campaign='sm120-mtp3-attribution', source_sha=source, source_clean=True, artifact_sha256=artifact, controller_id=controller, target_id=target, toolchain_image_id=image, counter_access_gate='q5-public', target_verify_width=4, profiles=['mtp3-engine-nsys', 'mtp3-native-round-timing', 'q4-m1-ncu', 'q4-m4-ncu', 'q5-public-ncu', 'q5-eager-cold', 'q5-graph-cold']), sort_keys=True) + '\n', encoding='utf-8')
PY
nvidia-smi --query-gpu=name,uuid,driver_version,temperature.gpu --format=csv >"$output_dir/environment/gpu-after.csv"
date -u +%Y-%m-%dT%H:%M:%SZ >"$output_dir/packet-complete.txt"
printf 'profile packet: %s\n' "$output_dir"
