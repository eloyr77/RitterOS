#!/bin/bash
# Builds the complete RitterOS toolchain inside <project>/tools.
# Nothing is installed outside the project folder.
#
#   tools/downloads  source tarballs / binary archives
#   tools/src        extracted sources and build directories
#   tools/venv       Python venv providing meson, ninja, pkgconf
#   tools/local      install prefix (bin/qemu-system-x86_64, share/qemu/edk2-*)
#   tools/zig        Zig toolchain (clang + lld, used as cross compiler)
#   tools/local/bin  also iasl and acpiexec (Intel's ACPICA, 37b): the
#                    tests' oracle for RitterOS's AML interpreter, never shipped
#   tools/src/atom-LINUX_VER  AMD's AtomBIOS interpreter from Linux (37g-5a):
#                    the oracle for amdgpu's, built only into scripts/atomhost
#                    on the Mac, never shipped
#
# Safe to re-run: each finished stage leaves a stamp file in tools/.stamps.
set -euo pipefail

QEMU_VER=11.1.2
GLIB_VER=2.90.1
PIXMAN_VER=0.46.4
SLIRP_VER=4.8.0
ZIG_VER=0.17.0
ACPICA_VER=20260930
ATOM_LINUX_VER=v6.6

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$ROOT/tools"
DL="$T/downloads"
SRC="$T/src"
PREFIX="$T/local"
STAMPS="$T/.stamps"
JOBS="$(sysctl -n hw.ncpu)"

mkdir -p "$DL" "$SRC" "$PREFIX" "$STAMPS"

log() { printf '\n=== %s ===\n' "$*"; }
done_stage() { touch "$STAMPS/$1"; }
is_done() { [ -f "$STAMPS/$1" ]; }

fetch() { # url
    local f="$DL/$(basename "$1")"
    [ -f "$f" ] || { curl -sSfL -o "$f.part" "$1" && mv "$f.part" "$f"; }
}

extract() { # archive dirname
    [ -d "$SRC/$2" ] || tar -xf "$DL/$1" -C "$SRC"
}

# --- Zig (cross C compiler + linker for UEFI / bare-metal x86_64) ---------
if ! is_done zig; then
    log "Zig $ZIG_VER"
    fetch "https://ziglang.org/download/$ZIG_VER/zig-aarch64-macos-$ZIG_VER.tar.xz"
    extract "zig-aarch64-macos-$ZIG_VER.tar.xz" "zig-aarch64-macos-$ZIG_VER"
    rm -rf "$T/zig" && mv "$SRC/zig-aarch64-macos-$ZIG_VER" "$T/zig"
    done_stage zig
fi

# --- Python venv with build tools ------------------------------------------
if ! is_done venv; then
    log "Python venv (meson, ninja, pkgconf, tomli)"
    python3 -m venv "$T/venv"
    "$T/venv/bin/pip" install -q --upgrade pip
    "$T/venv/bin/pip" install -q meson==1.11.2 ninja==1.13.2 pkgconf==2.4.3.post2 tomli==2.5.0
    done_stage venv
fi
export PATH="$PREFIX/bin:$T/venv/bin:$PATH"

# pkg-config wrapper: always resolve static link flags, only see our prefix.
mkdir -p "$PREFIX/bin"
cat > "$PREFIX/bin/pkg-config" <<EOF
#!/bin/sh
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
exec "$T/venv/bin/pkgconf" --static "\$@"
EOF
chmod +x "$PREFIX/bin/pkg-config"
export PKG_CONFIG="$PREFIX/bin/pkg-config"

meson_build() { # srcdir [meson options...]
    local s="$1"; shift
    ( cd "$SRC/$s" &&
      meson setup _build --prefix="$PREFIX" --libdir=lib --buildtype=release \
            --default-library=static --wrap-mode=default "$@" &&
      meson compile -C _build -j "$JOBS" &&
      meson install -C _build )
}

# --- pixman ----------------------------------------------------------------
if ! is_done pixman; then
    log "pixman $PIXMAN_VER"
    fetch "https://www.cairographics.org/releases/pixman-$PIXMAN_VER.tar.xz"
    extract "pixman-$PIXMAN_VER.tar.xz" "pixman-$PIXMAN_VER"
    rm -rf "$SRC/pixman-$PIXMAN_VER/_build"
    meson_build "pixman-$PIXMAN_VER" -Dtests=disabled -Ddemos=disabled -Dgtk=disabled -Dlibpng=disabled
    done_stage pixman
fi

# --- glib (pulls pcre2, libffi, proxy-libintl as meson subprojects) --------
if ! is_done glib; then
    log "glib $GLIB_VER"
    fetch "https://download.gnome.org/sources/glib/${GLIB_VER%.*}/glib-$GLIB_VER.tar.xz"
    extract "glib-$GLIB_VER.tar.xz" "glib-$GLIB_VER"
    rm -rf "$SRC/glib-$GLIB_VER/_build"
    meson_build "glib-$GLIB_VER" --force-fallback-for=pcre2,libffi,proxy-libintl \
        -Dtests=false -Dintrospection=disabled -Dman-pages=disabled \
        -Ddocumentation=false -Dsysprof=disabled -Dnls=disabled
    done_stage glib
fi

# --- libslirp (QEMU's "user" networking: DHCP, DNS, NAT without root) --------
if ! is_done slirp; then
    log "libslirp $SLIRP_VER"
    fetch "https://gitlab.freedesktop.org/slirp/libslirp/-/archive/v$SLIRP_VER/libslirp-v$SLIRP_VER.tar.gz"
    extract "libslirp-v$SLIRP_VER.tar.gz" "libslirp-v$SLIRP_VER"
    rm -rf "$SRC/libslirp-v$SLIRP_VER/_build"
    meson_build "libslirp-v$SLIRP_VER"
    done_stage slirp
fi

# --- QEMU (x86_64 and aarch64 system emulators) ------------------------------
# The stamp names the configuration: a toolchain built before libslirp
# (step 33) builds QEMU again.
if ! is_done qemu-slirp; then
    log "QEMU $QEMU_VER"
    fetch "https://download.qemu.org/qemu-$QEMU_VER.tar.xz"
    extract "qemu-$QEMU_VER.tar.xz" "qemu-$QEMU_VER"
    ( cd "$SRC/qemu-$QEMU_VER" && rm -rf build && mkdir build && cd build &&
      ../configure --prefix="$PREFIX" --python="$T/venv/bin/python3" \
          --target-list=x86_64-softmmu,aarch64-softmmu \
          --enable-cocoa --disable-docs --disable-werror \
          --disable-sdl --disable-gtk --disable-vnc --disable-curl \
          --disable-libssh --disable-gnutls --disable-nettle --disable-gcrypt \
          --disable-zstd --disable-libusb --disable-usb-redir --disable-spice \
          --enable-slirp --disable-slirp-smbd --disable-pvg &&
      make -j "$JOBS" && make install )
    done_stage qemu-slirp
fi

# --- Our own QEMU device: a model of Realtek's RTL8168 and RTL8125 ---------
# (step 42b; scripts/qemu/rtl8168.c): copied into QEMU's hw/net and built
# in. Copied and built again whenever the model changes.
Q="$SRC/qemu-$QEMU_VER"
if ! is_done qemu-rtl8168 || ! cmp -s "$ROOT/scripts/qemu/rtl8168.c" "$Q/hw/net/rtl8168.c"; then
    log "QEMU: the RTL8168/RTL8125 model"
    cp "$ROOT/scripts/qemu/rtl8168.c" "$Q/hw/net/rtl8168.c"
    grep -q CONFIG_RTL8168_PCI "$Q/hw/net/meson.build" ||
        echo "system_ss.add(when: 'CONFIG_RTL8168_PCI', if_true: files('rtl8168.c'))" \
            >>"$Q/hw/net/meson.build"
    grep -q "config RTL8168_PCI" "$Q/hw/net/Kconfig" ||
        printf '\nconfig RTL8168_PCI\n    bool\n    default y if PCI_DEVICES\n    depends on PCI\n' \
            >>"$Q/hw/net/Kconfig"
    ( cd "$Q/build" && make -j "$JOBS" && make install )
    done_stage qemu-rtl8168
fi

# --- Our own QEMU device: a simple Wi-Fi card (step 24d) -------------------
# (scripts/qemu/ritterwifi.c): our own invention, not a real chip, for the
# wireless-layer and wifid tests; built in like the Realtek model.
if ! is_done qemu-ritterwifi || ! cmp -s "$ROOT/scripts/qemu/ritterwifi.c" "$Q/hw/net/ritterwifi.c"; then
    log "QEMU: the RitterOS Wi-Fi model"
    cp "$ROOT/scripts/qemu/ritterwifi.c" "$Q/hw/net/ritterwifi.c"
    grep -q CONFIG_RITTERWIFI "$Q/hw/net/meson.build" ||
        echo "system_ss.add(when: 'CONFIG_RITTERWIFI', if_true: files('ritterwifi.c'))" \
            >>"$Q/hw/net/meson.build"
    grep -q "config RITTERWIFI" "$Q/hw/net/Kconfig" ||
        printf '\nconfig RITTERWIFI\n    bool\n    default y if PCI_DEVICES\n    depends on PCI\n' \
            >>"$Q/hw/net/Kconfig"
    ( cd "$Q/build" && make -j "$JOBS" && make install )
    done_stage qemu-ritterwifi
fi

# --- Our own QEMU device: a USB CDC-NCM Ethernet adapter --------------------
# (step 50a; scripts/qemu/usb_ncm.c): QEMU has only RNDIS and CDC-ECM USB-net
# devices, so we build in our own NCM model, like the Realtek one above.
if ! is_done qemu-usb-ncm || ! cmp -s "$ROOT/scripts/qemu/usb_ncm.c" "$Q/hw/usb/usb_ncm.c"; then
    log "QEMU: the USB CDC-NCM model"
    cp "$ROOT/scripts/qemu/usb_ncm.c" "$Q/hw/usb/usb_ncm.c"
    grep -q CONFIG_USB_NCM "$Q/hw/usb/meson.build" ||
        echo "system_ss.add(when: 'CONFIG_USB_NCM', if_true: files('usb_ncm.c'))" \
            >>"$Q/hw/usb/meson.build"
    grep -q "config USB_NCM" "$Q/hw/usb/Kconfig" ||
        printf '\nconfig USB_NCM\n    bool\n    default y\n    depends on USB\n' \
            >>"$Q/hw/usb/Kconfig"
    ( cd "$Q/build" && make -j "$JOBS" && make install )
    done_stage qemu-usb-ncm
fi

# --- Our own QEMU device: an ACPI embedded controller (37b-5) --------------
# (scripts/qemu/ritterec.c): the interface every embedded controller has
# (ACPI 6.5 chapter 12), for the tests of the kernel's driver; the test
# hands QEMU an SSDT that describes it (-acpitable).
if ! is_done qemu-ritterec || ! cmp -s "$ROOT/scripts/qemu/ritterec.c" "$Q/hw/misc/ritterec.c"; then
    log "QEMU: the embedded controller model"
    cp "$ROOT/scripts/qemu/ritterec.c" "$Q/hw/misc/ritterec.c"
    grep -q CONFIG_RITTEREC "$Q/hw/misc/meson.build" ||
        echo "system_ss.add(when: 'CONFIG_RITTEREC', if_true: files('ritterec.c'))" \
            >>"$Q/hw/misc/meson.build"
    grep -q "config RITTEREC" "$Q/hw/misc/Kconfig" ||
        printf '\nconfig RITTEREC\n    bool\n    default y if ISA_BUS\n    depends on ISA_BUS\n' \
            >>"$Q/hw/misc/Kconfig"
    ( cd "$Q/build" && make -j "$JOBS" && make install )
    done_stage qemu-ritterec
fi

# --- Intel's ACPICA tools: iasl and acpiexec (37b) -------------------------
# The oracle for RitterOS's own AML interpreter (roadmap item 37): iasl
# disassembles and compiles ACPI tables, acpiexec loads them and runs their
# methods. Built with the Mac's compiler (as QEMU is) and used on the Mac
# only; nothing of ACPICA goes into RitterOS (its licence: BSD or GPL v2).
if ! is_done acpica; then
    log "ACPICA $ACPICA_VER (iasl, acpiexec)"
    fetch "https://github.com/acpica/acpica/releases/download/$ACPICA_VER/acpica-unix-$ACPICA_VER.tar.gz"
    extract "acpica-unix-$ACPICA_VER.tar.gz" "acpica-unix-$ACPICA_VER"
    # (Apple's linker refuses the unaligned pointers in its packed debug
    # tables with fixup chains: linked without them, they only warn.)
    ( cd "$SRC/acpica-unix-$ACPICA_VER" &&
      make -j "$JOBS" iasl acpiexec LDFLAGS="-Wl,-no_fixup_chains" )
    cp "$SRC/acpica-unix-$ACPICA_VER/generate/unix/bin/iasl" \
       "$SRC/acpica-unix-$ACPICA_VER/generate/unix/bin/acpiexec" "$PREFIX/bin/"
    done_stage acpica
fi

# --- AMD's AtomBIOS interpreter from Linux (37g-5a) -----------------------
# The oracle for amdgpu's own (drivers/amdgpu_atom.c): atom.c and its
# headers (AMD's, MIT licence), at a pinned Linux tag. Only sources: the
# host harness scripts/atomhost.c builds them with its own stand-ins for
# the Linux kernel's headers. Nothing of it goes into RitterOS.
if ! is_done atom; then
    log "AtomBIOS interpreter of Linux $ATOM_LINUX_VER (the oracle)"
    A="$SRC/atom-$ATOM_LINUX_VER"
    mkdir -p "$A"
    for f in atom.c atom.h atom-bits.h atom-names.h atom-types.h atombios.h atomfirmware.h atomfirmwareid.h ObjectID.h; do
        case $f in atom.c|atom.h|ObjectID.h) dir=amdgpu ;; *) dir=include ;; esac
        curl -sSfL -o "$A/$f" \
            "https://raw.githubusercontent.com/torvalds/linux/$ATOM_LINUX_VER/drivers/gpu/drm/amd/$dir/$f"
    done
    done_stage atom
fi

log "Toolchain ready"
"$PREFIX/bin/qemu-system-x86_64" --version | head -1
echo "zig $("$T/zig/zig" version)"
