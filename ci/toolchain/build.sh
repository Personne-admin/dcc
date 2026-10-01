#!/usr/bin/env bash
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$here/../lib/toolchain.sh"

push=0
for arg in "$@"; do
    case "$arg" in
        --push) push=1 ;;
        --push-job)
            job_ref="$(job_image_ref)"
            docker build --build-arg "TOOLCHAIN_IMAGE=$TOOLCHAIN_REF${TOOLCHAIN_DIGEST:+@$TOOLCHAIN_DIGEST}" --tag "$job_ref" "$here/../job"
            docker push "$job_ref"
            exit 0 ;;
        --print-ref) echo "$TOOLCHAIN_REF"; exit 0 ;;
        *) echo "usage: $0 [--push|--push-job|--print-ref]" >&2; exit 2 ;;
    esac
done

args=()
while IFS='=' read -r key value; do
    [[ -z "$key" || "$key" == \#* ]] && continue
    args+=(--build-arg "$key=$value")
done < "$here/pins.env"

start=$(date +%s)
docker build "${args[@]}" --target toolchain --tag "$TOOLCHAIN_REF" "$here"
end=$(date +%s)
echo "built $TOOLCHAIN_REF in $((end - start))s"
docker image inspect "$TOOLCHAIN_REF" --format 'size {{.Size}} bytes'
docker run --rm "$TOOLCHAIN_REF" cat /opt/toolchain-manifest.txt

if [[ $push -eq 1 ]]; then
    docker push "$TOOLCHAIN_REF"
    digest="$(docker image inspect "$TOOLCHAIN_REF" --format '{{index .RepoDigests 0}}')"
    echo "${digest#*@}" > "$here/DIGEST"
    TOOLCHAIN_DIGEST="${digest#*@}"
    job_ref="$(job_image_ref)"
    docker build --build-arg "TOOLCHAIN_IMAGE=$TOOLCHAIN_REF@$TOOLCHAIN_DIGEST" --tag "$job_ref" "$here/../job"
    docker push "$job_ref"
    echo "pushed $TOOLCHAIN_REF@$TOOLCHAIN_DIGEST and $job_ref; commit ci/toolchain/DIGEST"
fi
