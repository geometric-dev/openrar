#!/usr/bin/env bash
# tools/provision_unrar.sh
# OpenRAR v2.1.0 Track E - Interop Oracle Provisioning (Linux)
# Downloads, verifies SHA-256, and provisions UnRAR 7.20 freeware for CI interop.

set -euo pipefail

UNRAR_VERSION="7.20"
# rarlab names the file without the dot (rarlinux-x64-720.tar.gz). SHA-256 measured from that
# download; the tarball ships rar/unrar and a whatsnew.txt whose newest entry is "Version 7.20".
UNRAR_SHA256="d3e7fba3272385b1d0255ee332a1e8c1a6779bb5a5ff9d4d8ac2be846e49ca46"
TAR_URL="https://www.rarlab.com/rar/rarlinux-x64-${UNRAR_VERSION//./}.tar.gz"
INSTALL_DIR="/usr/local/bin"

if command -v unrar >/dev/null 2>&1; then
    # The banner is preceded by a blank line, so take the first NON-EMPTY line. `|| true`
    # keeps `set -e/pipefail` from tripping on unrar's usage exit status or a closed pipe.
    VER=$(unrar 2>&1 | awk 'NF { print; exit }' || true)
    if echo "${VER}" | grep -q "UNRAR 7.20"; then
        echo "[ORACLE] Existing verified unrar 7.20 binary detected at: $(which unrar)"
        exit 0
    else
        echo "[ORACLE] Existing unrar is '${VER}', strictly UNRAR 7.20 required. Re-provisioning..."
    fi
fi

echo "[ORACLE] Downloading UnRAR ${UNRAR_VERSION} from ${TAR_URL}..."
TMP_DIR=$(mktemp -d)
trap 'rm -rf "${TMP_DIR}"' EXIT

curl -fsSL "${TAR_URL}" -o "${TMP_DIR}/rar.tar.gz"
echo "${UNRAR_SHA256}  ${TMP_DIR}/rar.tar.gz" | sha256sum -c -

tar -xzf "${TMP_DIR}/rar.tar.gz" -C "${TMP_DIR}"
if [ -w "${INSTALL_DIR}" ]; then
    cp "${TMP_DIR}/rar/unrar" "${INSTALL_DIR}/unrar"
    chmod +x "${INSTALL_DIR}/unrar"
else
    mkdir -p "${HOME}/.local/bin"
    cp "${TMP_DIR}/rar/unrar" "${HOME}/.local/bin/unrar"
    chmod +x "${HOME}/.local/bin/unrar"
    export PATH="${HOME}/.local/bin:${PATH}"
fi

echo "[ORACLE] UnRAR ${UNRAR_VERSION} provisioned successfully."

