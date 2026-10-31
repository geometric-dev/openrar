# OpenRAR Debian & Launchpad (PPA) Packaging Guide

This guide describes the tooling, packaging layout, and automated distribution pipeline for publishing OpenRAR as native Debian binary packages (`.deb`) and source packages (`.dsc`) to Canonical Launchpad Personal Package Archives (PPA).

---

## 1. Architecture & Design Principles

Canonical Launchpad PPAs do **not** accept pre-compiled binary `.deb` archives from untrusted clients. Instead, Launchpad requires cryptographically signed Debian source packages:
- `openrar_<version>.orig.tar.gz`: Pristine upstream source archive.
- `openrar_<version>-<release>.debian.tar.xz`: Debian packaging metadata and patches.
- `openrar_<version>-<release>.dsc`: Debian Source Control file with cryptographic hashes.
- `openrar_<version>-<release>_source.changes`: Source release record signed with a registered GPG key.

When uploaded via `dput`, Launchpad's automated build farm (Soyuz) unpacks the source, provisions clean isolated chroots across all targeted architectures (`amd64`, `arm64`, `armhf`, `ppc64el`, `s390x`, `riscv64`), executes `debian/rules`, runs the test suite, and publishes architecture-specific `.deb` binaries to the repository.

---

## 2. Multi-Binary Package Split

In accordance with Debian Policy (§8 Shared Libraries) and Ubuntu packaging standards, OpenRAR is split into three discrete binary packages:

| Package | Section | Contents | Purpose |
|---|---|---|---|
| **`openrar`** | `utils` | `/usr/bin/openrar`<br>`/usr/share/man/man1/openrar.1.gz` | Command-line archiver executable and manual page. |
| **`libopenrar1`** | `libs` | `/usr/lib/<triplet>/libopenrar.so.1*` | Shared library implementing the frozen C ABI (`SOVERSION 1`). Required at runtime by downstream consumers. |
| **`libopenrar-dev`** | `libdevel` | `/usr/include/openrar/*`<br>`/usr/lib/<triplet>/libopenrar.so`<br>`/usr/lib/<triplet>/libopenrar_core.a`<br>`/usr/lib/<triplet>/pkgconfig/openrar.pc`<br>`/usr/lib/<triplet>/cmake/openrar/*` | Development headers, CMake package config, pkg-config definition, and development symlinks. |

---

## 3. Debian Packaging Tree Structure

The `debian/` directory in the repository root contains the complete Debian packaging metadata:

```
debian/
├── changelog           # Debian changelog adhering to RFC 2822 formatting
├── control             # Source and binary package metadata, build-depends
├── copyright           # DEP-5 machine-readable copyright & licensing
├── libopenrar-dev.install  # Header, CMake, pkg-config, and .so symlink manifests
├── libopenrar1.install # Shared library runtime manifest (libopenrar.so.1*)
├── openrar.1           # Troff/groff UNIX manpage source
├── openrar.install     # Binary CLI executable manifest
├── openrar.manpages    # Manpage registration
├── rules               # Executable Makefile orchestrating CMake via debhelper
├── source/
│   ├── format          # Source package format: "3.0 (quilt)"
│   └── options         # Tarball packaging options and diff filters
└── watch               # UScan watchfile tracking upstream GitHub releases
```

---

## 4. Local Build & Verification

### 4.1 Verification Gate

To verify that the packaging metadata is complete, valid, and synchronized with project versioning across all language bindings:

```bash
python tools/packaging/launchpad_release.py --verify-only
```

### 4.2 Building Source Packages Locally

On Ubuntu / Debian / WSL:

```bash
# Install packaging tools
sudo apt-get install -y debhelper devscripts dpkg-dev lintian

# Build unsigned source package for Ubuntu 24.04 (Noble)
./tools/packaging/build_deb_source.sh --distro noble --output-dir build/debian-source

# Or build and sign with your GPG key ID
./tools/packaging/build_deb_source.sh --distro noble --output-dir build/debian-source --sign YOUR_GPG_KEY_ID
```

The output artifacts will be placed in `build/debian-source/`:
- `openrar_1.53.0.orig.tar.gz`
- `openrar_1.53.0-0ubuntu1~noble1.debian.tar.xz`
- `openrar_1.53.0-0ubuntu1~noble1.dsc`
- `openrar_1.53.0-0ubuntu1~noble1_source.changes`

### 4.3 Testing Binary Package Build (Docker or Local)

To test a full binary build locally:

```bash
cd build/debian-source/openrar-1.53.0
dpkg-buildpackage -b -us -uc

# Inspect generated binary packages
ls -lh ../*.deb
```

---

## 5. Setting Up Canonical Launchpad PPA

### 5.1 GPG Key Setup

Launchpad requires that source changes files (`_source.changes`) be signed by a GPG key associated with your Launchpad account.

1. Generate a 4096-bit RSA key:
   ```bash
   gpg --full-generate-key
   ```
2. Export your public key to the Ubuntu keyserver:
   ```bash
   gpg --keyserver keyserver.ubuntu.com --send-keys <YOUR_KEY_FINGERPRINT>
   ```
3. In your Launchpad account settings (**OpenPGP keys**), enter your key fingerprint. Launchpad will send an encrypted confirmation email; decrypt it with `gpg --decrypt` to complete registration.

### 5.2 Configuring dput

Create or update `~/.dput.cf`:

```ini
[ppa-geometric-dev]
fqdn = ppa.launchpad.net
method = ftp
incoming = ~geometric-dev/ubuntu/openrar/
login = anonymous
allow_unsigned_uploads = 0
```

### 5.3 Manual Upload

```bash
dput ppa:geometric-dev/openrar build/debian-source/openrar_1.53.0-0ubuntu1~noble1_source.changes
```

---

## 6. Automated GitHub Actions Workflow

OpenRAR provides automated Launchpad publishing via [`.github/workflows/launchpad.yml`](file:///.github/workflows/launchpad.yml).

### 6.1 Configuration

Add your armored ASCII GPG private key to GitHub Repository Secrets:
- Secret name: `LAUNCHPAD_GPG_KEY`
- Content: Output of `gpg --armor --export-secret-keys <YOUR_KEY_FINGERPRINT>`

### 6.2 Triggering

- **Automatic:** Pushing a release tag `v*` (e.g. `v1.53.0`) triggers the workflow automatically.
- **Manual:** Navigate to **Actions** -> **Launchpad PPA (deb)** -> **Run workflow**, where you can select target Ubuntu series (e.g. `resolute,noble,jammy`) or enable a `dry_run` validation build without uploading.

---

## 7. End-User Installation

Once built on Launchpad, Ubuntu and Debian-derived users install OpenRAR with:

```bash
# Add official OpenRAR PPA
sudo add-apt-repository ppa:geometric-dev/openrar
sudo apt update

# Install OpenRAR CLI utility and manual page
sudo apt install openrar

# (Optional) Install development headers and frozen C ABI library
sudo apt install libopenrar-dev
```

### Verifying Installation

```bash
# Verify CLI
openrar -version
man openrar

# Verify pkg-config metadata
pkg-config --modversion openrar
pkg-config --cflags --libs openrar
```
