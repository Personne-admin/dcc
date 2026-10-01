
toolchain_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../toolchain" && pwd)"

set -a
source "$toolchain_dir/pins.env"
set +a

TOOLCHAIN_REPO="${TOOLCHAIN_REPO:-ghcr.io/personne-admin/dcc-toolchain}"
TOOLCHAIN_TAG="llvm${LLVM_VERSION}-mingw${LLVM_MINGW_RELEASE}-r${RECIPE_REVISION}"
TOOLCHAIN_REF="$TOOLCHAIN_REPO:$TOOLCHAIN_TAG"
TOOLCHAIN_DIGEST=""

if [[ -s "$toolchain_dir/DIGEST" ]]; then
    TOOLCHAIN_DIGEST="$(cat "$toolchain_dir/DIGEST")"
fi
JOB_REPO="${JOB_REPO:-ghcr.io/personne-admin/dcc-ci-job}"

job_image_ref() {
    local hash
    hash="$( { echo "$TOOLCHAIN_REF@$TOOLCHAIN_DIGEST"; cat "$toolchain_dir/../job/Dockerfile"; } | sha256sum | cut -c1-16)"
    echo "$JOB_REPO:$hash"
}
