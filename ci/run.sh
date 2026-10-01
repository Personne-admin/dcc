#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat >&2 <<'USAGE'
usage: ci/run.sh [options] <job>... | all
       ci/run.sh --print-toolchain-image | --print-job-image

options:
  --commit REV      commit to check out (default HEAD)
  --cpus N          limit each job container to N CPUs (docker --cpus)
  --timeout SECS    per-invocation compiler timeout (default 60, DCC_TIMEOUT)
  --dry-run         release only: build with version 0.0.0-dryrun instead of a tag
  --tag vX.Y.Z      release only: the release tag (default: tag pointing at REV)
  --out DIR         release only: where to copy artifacts (default ./dist/release)
  --keep            keep the workspace after the job
  --no-pull         never pull images; use local ones and build the job image locally

environment:
  DCC_CI_WORKDIR    where workspaces are created (default ~/.cache/dcc-ci)
  DCC_CI_CACHE_DIR  host directory mounted at /cache for the npm cache
USAGE
    exit 2
}

ci_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$ci_dir/.." && pwd)"
source "$ci_dir/lib/toolchain.sh"

commit=HEAD
cpus=""
timeout="${DCC_TIMEOUT:-60}"
dry_run=0
tag=""
out="$repo/dist/release"
keep=0
pull=1
jobs=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --commit) commit="$2"; shift 2 ;;
        --cpus) cpus="$2"; shift 2 ;;
        --timeout) timeout="$2"; shift 2 ;;
        --dry-run) dry_run=1; shift ;;
        --tag) tag="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --keep) keep=1; shift ;;
        --no-pull) pull=0; shift ;;
        --print-toolchain-image) echo "$TOOLCHAIN_REF${TOOLCHAIN_DIGEST:+@$TOOLCHAIN_DIGEST}"; exit 0 ;;
        --print-job-image) print_job=1; shift ;;
        -h|--help) usage ;;
        -*) echo "unknown option $1" >&2; usage ;;
        *) jobs+=("$1"); shift ;;
    esac
done

all_jobs=(lint vscode benchmark-gate static unit linux windows differential)
JOB_IMAGE="$(job_image_ref)"

if [[ "${print_job:-0}" -eq 1 ]]; then
    echo "$JOB_IMAGE"
    exit 0
fi

[[ ${#jobs[@]} -gt 0 ]] || usage
if [[ "${jobs[*]}" == "all" ]]; then
    jobs=("${all_jobs[@]}")
fi
for job in "${jobs[@]}"; do
    [[ -x "$ci_dir/jobs/$job.sh" ]] || { echo "unknown job $job" >&2; usage; }
done

toolchain_image="$TOOLCHAIN_REF${TOOLCHAIN_DIGEST:+@$TOOLCHAIN_DIGEST}"
if ! docker image inspect "$toolchain_image" >/dev/null 2>&1; then
    if [[ $pull -eq 1 ]]; then
        docker pull "$toolchain_image"
    else
        echo "toolchain image $toolchain_image is not present; run ci/toolchain/build.sh" >&2
        exit 1
    fi
fi

if ! docker image inspect "$JOB_IMAGE" >/dev/null 2>&1; then
    if [[ $pull -eq 0 ]] || ! docker pull "$JOB_IMAGE"; then
        docker build --build-arg "TOOLCHAIN_IMAGE=$toolchain_image" --tag "$JOB_IMAGE" "$ci_dir/job"
    fi
fi

sha="$(git -C "$repo" rev-parse --verify "$commit^{commit}")"
work_root="${DCC_CI_WORKDIR:-${RUNNER_TEMP:-${XDG_CACHE_HOME:-$HOME/.cache}}/dcc-ci}"
mkdir -p "$work_root"

declare -A durations
declare -A results
overall=0

for job in "${jobs[@]}"; do
    ws="$(mktemp -d "$work_root/$job-${sha:0:12}-XXXXXX")"
    git clone --quiet --no-hardlinks --no-checkout "$repo" "$ws/src"
    git -C "$ws/src" checkout --quiet --detach "$sha"
    mkdir -p "$ws/home" "$ws/out"

    run_args=(--rm --init --network host
        --user "$(id -u):$(id -g)"
        -v "$ws:/work" -w /work/src
        -e HOME=/work/home -e DCC_TIMEOUT="$timeout" -e CI_JOB="$job"
        -e DCC_RELEASE_DRY_RUN="$dry_run" -e DCC_RELEASE_TAG="$tag" -e DCC_CI_OUT=/work/out)

    if [[ -n "${DCC_CI_CACHE_DIR:-}" ]]; then
        mkdir -p "$DCC_CI_CACHE_DIR"
        run_args+=(-v "$(realpath "$DCC_CI_CACHE_DIR"):/cache" -e npm_config_cache=/cache/npm)
    fi
    if [[ -n "$cpus" ]]; then
        run_args+=(--cpus "$cpus" -e CI_JOBS="${cpus%.*}")
    fi
    if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
        run_args+=(-e GITHUB_ACTIONS)
    fi

    echo "==> $job (${sha:0:12}) in $ws"
    start=$(date +%s)
    if docker run "${run_args[@]}" "$JOB_IMAGE" "ci/jobs/$job.sh"; then
        results[$job]=PASS
    else
        results[$job]=FAIL
        overall=1
    fi
    durations[$job]=$(( $(date +%s) - start ))
    echo "==> $job ${results[$job]} in ${durations[$job]}s"

    if [[ "$job" == release && "${results[$job]}" == PASS ]]; then
        mkdir -p "$out"
        cp -a "$ws/out/." "$out/"
        echo "==> release artifacts in $out"
    fi
    if [[ $keep -eq 0 && "${results[$job]}" == PASS ]]; then
        chmod -R u+w "$ws" 2>/dev/null || true
        rm -rf "$ws"
    else
        echo "==> workspace kept: $ws"
    fi
done

echo
printf '%-16s %-6s %s\n' JOB RESULT SECONDS
for job in "${jobs[@]}"; do
    printf '%-16s %-6s %s\n' "$job" "${results[$job]}" "${durations[$job]}"
done
exit $overall
