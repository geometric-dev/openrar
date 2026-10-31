#!/usr/bin/env bash
# tools/packaging/build_deb_source.sh
# Build Debian/Ubuntu source package for Launchpad PPA upload.
#
# Generates:
#   openrar_<version>.orig.tar.gz
#   openrar_<version>-0ubuntu1~<series>1.debian.tar.xz
#   openrar_<version>-0ubuntu1~<series>1.dsc
#   openrar_<version>-0ubuntu1~<series>1_source.changes
#
# Usage:
#   ./tools/packaging/build_deb_source.sh [--distro noble] [--output-dir ../ppa-build] [--sign KEY_ID]

set -euo pipefail

DISTRO="noble"
OUTPUT_DIR="build/debian-source"
KEY_ID=""
DO_SIGN=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --distro)
            DISTRO="$2"
            shift 2
            ;;
        --output-dir)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --sign)
            KEY_ID="$2"
            DO_SIGN=true
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [--distro <series>] [--output-dir <dir>] [--sign <KEY_ID>]"
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

cd "${REPO_ROOT}"

# Extract version from CMakeLists.txt
VERSION=$(grep -E 'project\(openrar VERSION [0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt | sed -E 's/.*VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/')
if [[ -z "${VERSION}" ]]; then
    echo "Error: Failed to extract VERSION from CMakeLists.txt" >&2
    exit 1
fi

echo "==> Preparing Debian source package for OpenRAR v${VERSION} (${DISTRO})"

# Create output staging directory
mkdir -p "${OUTPUT_DIR}"
STAGING_DIR="${OUTPUT_DIR}/openrar-${VERSION}"
rm -rf "${STAGING_DIR}"
mkdir -p "${STAGING_DIR}"

# 1. Export clean source tree without VCS or temporary artifacts
echo "==> Creating upstream orig tarball openrar_${VERSION}.orig.tar.gz..."
ORIG_TARBALL="${OUTPUT_DIR}/openrar_${VERSION}.orig.tar.gz"

git archive --format=tar.gz --prefix="openrar-${VERSION}/" -o "${ORIG_TARBALL}" HEAD

# 2. Extract orig tarball into staging directory
tar -xzf "${ORIG_TARBALL}" -C "${OUTPUT_DIR}"

# 3. Ensure debian/ directory is copied into staging
cp -r "${REPO_ROOT}/debian" "${STAGING_DIR}/"

# 4. Generate distribution-specific changelog entry if DISTRO differs from changelog
cd "${STAGING_DIR}"
DEBIAN_VERSION="${VERSION}-0ubuntu2~${DISTRO}1"

cat <<EOF > debian/changelog
openrar (${DEBIAN_VERSION}) ${DISTRO}; urgency=medium

  * Automated Launchpad PPA build for Ubuntu ${DISTRO}.
  * Upstream OpenRAR release v${VERSION}.
  * Multi-binary split: openrar, libopenrar1, libopenrar-dev.

 -- OpenRAR Project <geometric-dev@users.noreply.github.com>  $(date -R)
EOF

# 5. Build source package
echo "==> Running dpkg-source / dpkg-buildpackage -S..."
if [[ "${DO_SIGN}" = true && -n "${KEY_ID}" ]]; then
    dpkg-buildpackage -S -sa -d -k"${KEY_ID}"
else
    dpkg-buildpackage -S -sa -d -us -uc
fi

cd "${REPO_ROOT}"
echo "==> Debian source package built successfully in ${OUTPUT_DIR}:"
ls -lh "${OUTPUT_DIR}"/*.dsc "${OUTPUT_DIR}"/*.changes 2>/dev/null || true
