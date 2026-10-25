#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
  echo "usage: $0 CONFIG CANDIDATE_VARIANT" >&2
  exit 2
}

[[ $# -eq 2 ]] || usage
config=$1
candidate=$2
[[ -f "$config" ]] || { echo "profile config not found: $config" >&2; exit 2; }
set -a
# The configuration is a trusted, operator-owned shell environment file.
# shellcheck source=/dev/null
source "$config"
set +a

: "${PROFILE_RESULT_DIR:?profile config is missing PROFILE_RESULT_DIR}"
: "${PROFILE_ARTIFACT_ROOT:?profile config is missing PROFILE_ARTIFACT_ROOT}"
: "${PROFILE_ARTIFACT_MANIFEST:?profile config is missing PROFILE_ARTIFACT_MANIFEST}"
[[ $candidate =~ ^[a-zA-Z0-9][a-zA-Z0-9_.-]*$ ]] || {
  echo "invalid candidate variant: $candidate" >&2
  exit 2
}
[[ $candidate != baseline ]] || { echo "candidate variant must not be baseline" >&2; exit 2; }
[[ -x "$PROFILE_ARTIFACT_ROOT/baseline/ninfer-serve" ]] || {
  echo "baseline server artifact is missing" >&2
  exit 2
}
[[ -x "$PROFILE_ARTIFACT_ROOT/$candidate/ninfer-serve" ]] || {
  echo "candidate server artifact is missing: $candidate" >&2
  exit 2
}
mkdir -p "$PROFILE_RESULT_DIR/e2e"
(
  cd "$PROFILE_ARTIFACT_ROOT"
  sha256sum --check "$PROFILE_ARTIFACT_MANIFEST"
) > "$PROFILE_RESULT_DIR/artifact-manifest-check.txt"

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
cycle="$script_dir/run_long_prefill_mtp3_cycle.sh"
printf '%s\t%s\n' \
  baseline baseline-a \
  "$candidate" candidate-a \
  "$candidate" candidate-b \
  baseline baseline-b \
  baseline baseline-c \
  "$candidate" candidate-c \
  > "$PROFILE_RESULT_DIR/e2e/order.tsv"

while IFS=$'\t' read -r variant label; do
  "$cycle" "$config" "$variant" "$label" \
    > "$PROFILE_RESULT_DIR/e2e/$label-$variant.log" 2>&1
done < "$PROFILE_RESULT_DIR/e2e/order.tsv"
