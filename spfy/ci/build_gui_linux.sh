#!/usr/bin/env bash
# Build the Tauri GUI for Linux.
#
# Run alongside spfy/ci/build_unix.sh, not instead of it: that one builds the
# C engine, this one builds the desktop shell that spawns it. They are split
# because they need completely different toolchains and only some legs get a
# GUI.
#
#   SPFY_GUI_OUT     directory to write the finished spfy_gui into (required)
#   SPFY_MAX_GLIBC   highest glibc version the result may reference (optional;
#                    same meaning as in build_unix.sh)
#   SPFY_GUI_CROSS   set to an armhf-style Debian arch (currently only
#                    "armhf") to cross-compile against a sysroot instead of
#                    building natively. Unset = native build.
#
# ⛔ NOT RUN ON THE musl LEGS. Those tarballs are statically linked so they run
# on Alpine and in FROM-scratch containers with no libc question at all.
# WebKitGTK ships no static archives on Alpine and cannot practically be
# statically linked anywhere, so a GUI there would be a dynamically linked
# binary needing ~40 shared libraries -- the exact property those legs exist
# to not have. They stay CLI-only.
set -eu

: "${SPFY_GUI_OUT:?set SPFY_GUI_OUT to the directory to write spfy_gui into}"

HERE="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="${HERE}/spfy/gui/src-tauri"
CROSS="${SPFY_GUI_CROSS:-}"

# libwebkit2gtk-4.1 is the webview Tauri v2 links against on Linux. bookworm
# carries 2.50.x for every architecture this project builds, which is what
# keeps the GUI inside the SAME glibc ceiling as the CLI beside it.
GUI_DEPS="libwebkit2gtk-4.1-dev libgtk-3-dev librsvg2-dev libsoup-3.0-dev
          libssl-dev libdbus-1-dev libsystemd-dev"

export DEBIAN_FRONTEND=noninteractive
export PATH="/root/.cargo/bin:$PATH"

install_rust() {   # <host-triple> [extra-target]
    # bookworm's own rustc is 1.63; Cargo.toml needs 1.77 and so does Tauri 2,
    # so the distro toolchain is not an option.
    curl -sSf https://sh.rustup.rs -o /tmp/rustup.sh
    sh /tmp/rustup.sh -y --profile minimal --default-toolchain stable \
        --default-host "$1" >/dev/null 2>&1
    if [ -n "${2:-}" ]; then
        rustup target add "$2" >/dev/null
    fi
    rustc --version
}

if [ -z "${CROSS}" ]; then
    # ---------------- native ----------------
    echo "=== GUI: apt dependencies (native) ==="
    apt-get update -qq
    # shellcheck disable=SC2086
    apt-get install -y --no-install-recommends \
        build-essential curl ca-certificates pkg-config file binutils \
        ${GUI_DEPS} >/dev/null
    DPKG_ARCH="$(dpkg --print-architecture)"
    echo "    userland: ${DPKG_ARCH}"

    echo "=== GUI: rust toolchain ==="
    # ⛔ --default-host IS LOAD-BEARING, AND THE FAILURE IS OPAQUE WITHOUT IT.
    # rustup derives the host triple from `uname -m`, which reports the
    # KERNEL. In an i386 container on an x86_64 host that is "x86_64", so
    # rustup installs an x86_64 toolchain the 32-bit userland has no loader
    # for, and every command then dies with
    #     error: command failed: 'rustc': No such file or directory
    # against a file that is plainly present. Asking dpkg instead asks the
    # USERLAND, which is the thing that actually has to run it.
    case "${DPKG_ARCH}" in
        amd64) RUST_HOST=x86_64-unknown-linux-gnu ;;
        i386)  RUST_HOST=i686-unknown-linux-gnu ;;
        arm64) RUST_HOST=aarch64-unknown-linux-gnu ;;
        armhf) RUST_HOST=armv7-unknown-linux-gnueabihf ;;
        *)     echo "unsupported dpkg architecture: ${DPKG_ARCH}" >&2; exit 1 ;;
    esac
    echo "    host triple: ${RUST_HOST}"
    install_rust "${RUST_HOST}"

    echo "=== GUI: build ==="
    export CARGO_TARGET_DIR="${SPFY_GUI_OUT}/cargo-target"
    cd "${SRC}"
    cargo build --release --locked
    BIN="${CARGO_TARGET_DIR}/release/spfy_gui"
    OBJDUMP=objdump
else
    # ---------------- cross, against a sysroot ----------------
    #
    # ⛔ THE TWO OBVIOUS WAYS BOTH FAIL, so neither is worth re-trying:
    #   1. Debian multi-arch (:armhf dev packages on an amd64 root):
    #      libwebkit2gtk-4.1-dev:armhf needs gobject-introspection:armhf ->
    #      python3:armhf / python3-mako:armhf, which are not co-installable
    #      for a foreign arch. apt reports only "held broken packages".
    #   2. Building natively in a qemu-emulated armhf container: measured,
    #      apt alone ran past THIRTY MINUTES before rustup started. The leg
    #      would not finish inside any sane timeout.
    #
    # mmdebstrap --variant=extract unpacks the packages without configuring
    # them, so no maintainer script ever runs and no emulation is needed --
    # 31.5s measured. A sysroot is only headers, .so files and .pc files;
    # nothing in it has to execute on the build machine.
    SYSROOT=/tmp/spfy-sysroot-${CROSS}
    case "${CROSS}" in
        armhf)
            GNU_TRIPLE=arm-linux-gnueabihf
            RUST_TARGET=armv7-unknown-linux-gnueabihf
            RUST_TARGET_ENV=ARMV7_UNKNOWN_LINUX_GNUEABIHF
            ;;
        *) echo "unsupported SPFY_GUI_CROSS: ${CROSS}" >&2; exit 1 ;;
    esac

    echo "=== GUI: host tools ==="
    apt-get update -qq
    # ⚠ build-essential (HOST) as well as the cross toolchain. Cargo builds
    # every proc-macro and build.rs FOR THE HOST even in a cross build, so
    # without a host `cc` the first crate dies with "linker `cc` not found",
    # which reads like a cross-compilation fault and is not one.
    apt-get install -y --no-install-recommends \
        mmdebstrap curl ca-certificates file build-essential symlinks \
        pkg-config "crossbuild-essential-${CROSS}" >/dev/null

    echo "=== GUI: ${CROSS} sysroot (extract-only, no emulation) ==="
    # shellcheck disable=SC2086
    mmdebstrap --architectures="${CROSS}" --variant=extract \
        --include="$(echo ${GUI_DEPS} | tr ' ' ','),libc6-dev,linux-libc-dev" \
        bookworm "${SYSROOT}" http://deb.debian.org/debian 2>&1 | tail -2

    # ⛔ DEBIAN'S -dev .so SYMLINKS ARE ABSOLUTE, and inside a sysroot that is
    # fatal in a way that does not look like a symlink problem. libdbus-1.so
    # -> /lib/arm-linux-gnueabihf/libdbus-1.so.3 resolves against the HOST
    # root, not ${SYSROOT}, so ld cannot see the shared library, silently
    # falls back to libdbus-1.a, and then dies on the static archive's own
    # dependency:
    #     undefined reference to symbol 'sd_is_socket@@LIBSYSTEMD_209'
    #     libsystemd.so.0: error adding symbols: DSO missing from command line
    # Making them relative is what keeps the sysroot self-contained.
    symlinks -cr "${SYSROOT}" >/dev/null 2>&1 || true
    echo "    sysroot: $(du -sh "${SYSROOT}" | cut -f1), symlinks made relative"

    echo "=== GUI: rust toolchain ==="
    install_rust x86_64-unknown-linux-gnu "${RUST_TARGET}"

    echo "=== GUI: cross build for ${RUST_TARGET} ==="
    # pkg-config must answer for the SYSROOT, not the amd64 host.
    # PKG_CONFIG_SYSROOT_DIR prefixes the paths it prints; PKG_CONFIG_LIBDIR
    # REPLACES the search path rather than adding to it, so the host's own .pc
    # files cannot leak in and hand the cross-linker amd64 objects.
    export PKG_CONFIG_ALLOW_CROSS=1
    export PKG_CONFIG_SYSROOT_DIR="${SYSROOT}"
    export PKG_CONFIG_LIBDIR="${SYSROOT}/usr/lib/${GNU_TRIPLE}/pkgconfig:${SYSROOT}/usr/share/pkgconfig"
    # ⚠ -lsystemd IS EXPLICIT ON PURPOSE. ld resolves libdbus-1 to the STATIC
    # archive in an extracted sysroot, and that archive references
    # sd_is_socket from libsystemd. ld then finds libsystemd.so.0 as an
    # indirect dependency but refuses it with
    #     libsystemd.so.0: error adding symbols: DSO missing from command line
    # because modern ld will not link against a library it was not told about.
    # Naming it is the documented resolution; the alternative
    # (--copy-dt-needed-entries) is deprecated and papers over the same thing.
    export "CARGO_TARGET_${RUST_TARGET_ENV}_LINKER=${GNU_TRIPLE}-gcc"
    export "CARGO_TARGET_${RUST_TARGET_ENV}_RUSTFLAGS=-C link-arg=--sysroot=${SYSROOT} -C link-arg=-lsystemd"
    export CARGO_TARGET_DIR="${SPFY_GUI_OUT}/cargo-target"
    cd "${SRC}"
    cargo build --release --locked --target "${RUST_TARGET}"
    BIN="${CARGO_TARGET_DIR}/${RUST_TARGET}/release/spfy_gui"
    OBJDUMP="${GNU_TRIPLE}-objdump"
fi

test -f "${BIN}" || { echo "no spfy_gui at ${BIN}" >&2; exit 1; }

echo "=== GUI: result ==="
ls -la "${BIN}"
file "${BIN}"

# ⚠ Same ceiling the CLI is held to. A GUI that demands a newer glibc than the
# engine beside it makes the tarball unportable in exactly the way
# SPFY_MAX_GLIBC exists to prevent -- and it would do it silently, because the
# CLI would keep working on the machines the GUI had just stopped starting on.
if [ -n "${SPFY_MAX_GLIBC:-}" ]; then
    echo "=== GUI: glibc ceiling (max ${SPFY_MAX_GLIBC}) ==="
    HIGHEST=$("${OBJDUMP}" -T "${BIN}" 2>/dev/null |
        grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sed 's/GLIBC_//' | sort -u -V | tail -1)
    echo "    highest referenced: ${HIGHEST:-none}"
    if [ -n "${HIGHEST}" ]; then
        LOWEST=$(printf '%s\n%s\n' "${HIGHEST}" "${SPFY_MAX_GLIBC}" | sort -V | head -1)
        if [ "${LOWEST}" != "${HIGHEST}" ]; then
            echo "::error::spfy_gui needs glibc ${HIGHEST}, ceiling is ${SPFY_MAX_GLIBC}"
            exit 1
        fi
    fi
fi

mkdir -p "${SPFY_GUI_OUT}"
cp "${BIN}" "${SPFY_GUI_OUT}/spfy_gui"
echo "wrote ${SPFY_GUI_OUT}/spfy_gui"
