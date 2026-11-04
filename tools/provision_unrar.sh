#!/usr/bin/env bash
# tools/provision_unrar.sh
# OpenRAR v2.1.0 Track E - Interop Oracle Provisioning (Linux)
# Downloads, verifies SHA-256, and provisions UnRAR 7.20 freeware for CI interop.

set -euo pipefail

UNRAR_VERSION="7.20"
UNRAR_SHA256="6b541334969bb128d54d588523c10a4dfb7d80f8ce91500f4f5a34e568bb9e60"
TAR_URL="https://www.rarlab.com/rar/rarlinux-x64-${UNRAR_VERSION}.tar.gz"
INSTALL_DIR="/usr/local/bin"

if command -v unrar >/dev/null 2>&1; then
    echo "[ORACLE] Existing unrar binary detected at: $(which unrar)"
    unrar | head -n 2
    exit 0
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

