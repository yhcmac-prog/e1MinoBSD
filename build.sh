#!/bin/sh
# build.sh — e1MinoBSD build script
# Stages: fetch -> (later tasks) stage -> iso
set -eu
cd "$(dirname "$0")"

ROOT=$(pwd)
DL=downloads
BUILD=build
FBSD_VER=14.5-RELEASE
DIST_REL=releases/amd64/amd64/$FBSD_VER

msg() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die() { printf '\033[31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- variant
# E1BSD_VARIANT=minimal (default) / desktop (5 desktop environments)
VARIANT=${E1BSD_VARIANT:-minimal}
case "$VARIANT" in
    minimal|desktop) ;;
    *) die "E1BSD_VARIANT must be minimal or desktop" ;;
esac
case "$VARIANT" in
    minimal) VOLLABEL=E1MINOBSD10_MIN ;;
    desktop) VOLLABEL=E1MINOBSD10_DSK ;;
esac

# E1BSD_AUTOSETUP=ufs|zfs  builds an ISO that auto-installs on first boot
AUTOSETUP=${E1BSD_AUTOSETUP:-}
case "$AUTOSETUP" in
    ""|ufs|zfs) ;;
    *) die "E1BSD_AUTOSETUP must be ufs or zfs" ;;
esac

FETCH_ONLY=0
STAGE_ONLY=0
for a in "$@"; do
    case "$a" in
        --fetch-only) FETCH_ONLY=1 ;;
        --stage-only) STAGE_ONLY=1 ;;
        *) die "unknown argument: $a" ;;
    esac
done

for c in python3 curl tar xz; do
    command -v "$c" >/dev/null 2>&1 || die "missing dependency: $c"
done

mkdir -p "$DL" "$BUILD"

# ---------------------------------------------------------------- download
# Parallel ranged download with mirror fallback (Aliyun -> FreeBSD.org)
pfetch() { # <relpath> <out> [mirrors...]
    rel=$1; out=$2; shift 2
    urls=""
    for m in "$@"; do urls="$urls $m/$rel"; done
    # pfetch itself verifies size and repairs truncated files
    python3 tools/pfetch.py $urls -o "$out" || die "download failed: $rel"
}

M_ALI=https://mirrors.aliyun.com/freebsd
M_ORG=https://download.freebsd.org

msg "stage: fetch FreeBSD $FBSD_VER distribution sets"
pfetch "$DIST_REL/base.txz"     "$DL/base.txz" "$M_ALI" "$M_ORG"
pfetch "$DIST_REL/kernel.txz"   "$DL/kernel.txz" "$M_ALI" "$M_ORG"
# /boot/cdboot is included in base.txz (verified identical to ISO copy).

# ---------------------------------------------------------------- cdrtools
# Vendored in-project copy of cdrtools (mkisofs/isoinfo), from e1LibreOS.
if [ ! -x tools/cdrtools/3.02a09/bin/mkisofs ]; then
    msg "vendor cdrtools into tools/cdrtools"
    [ -d ../e1LibreOS/brew/cdrtools ] \
        || die "e1LibreOS cdrtools not found"
    mkdir -p tools/cdrtools
    cp -R ../e1LibreOS/brew/cdrtools/3.02a09 tools/cdrtools/
fi
tools/cdrtools/3.02a09/bin/mkisofs -version >/dev/null 2>&1 \
    || die "mkisofs not runnable"

if [ "$FETCH_ONLY" = 1 ]; then
    msg "fetch stage complete:"
    ls -lh "$DL/base.txz" "$DL/kernel.txz" \
        | awk '{printf "    %-12s %s\n", $5, $NF}'
    exit 0
fi

# ------------------------------------------------------------- packages
# Build sample e1pkg packages and the local repository index.
REPO=$BUILD/repo
build_pkgs() {
    msg "stage: build e1pkg packages"
    rm -rf "$REPO"
    mkdir -p "$REPO/packages"
    : > "$REPO/repo.db"
    for d in $(ls -d packages/src/*/ | sort); do
        [ -f "$d/meta" ] || continue
        name=$(grep '^name='    "$d/meta" | cut -d= -f2-)
        ver=$(grep  '^version=' "$d/meta" | cut -d= -f2-)
        sum=$(grep  '^summary=' "$d/meta" | cut -d= -f2-)
        dep=$(grep  '^depends=' "$d/meta" | cut -d= -f2-)
        find "$d/payload" -type f -exec chmod +x {} \; 2>/dev/null || true
        fname="$name-$ver.e1.tar.gz"
        tar -czf "$REPO/packages/$fname" -C "$d" meta payload
        printf '%s|%s|%s|%s|%s\n' "$name" "$ver" "$sum" "$dep" "$fname" \
            >> "$REPO/repo.db"
    done
    [ -s "$REPO/repo.db" ] || die "no packages found"
}
build_pkgs

# Desktop closure must be present before staging (checked below).

# Stage content hash: any change in overlays or package sources invalidates it.
CONTENT_HASH=$({ cat "$0"; \
                tar cf - rootfs rootfs-desktop packages/src tools; \
                cat "$REPO/repo.db"; \
                [ -s "$BUILD/packages/pkg-list.txt" ] \
                    && cat "$BUILD/packages/pkg-list.txt"; \
                echo "autosetup=$AUTOSETUP"; } \
    | shasum | cut -d' ' -f1)

# ---------------------------------------------------------------- stage
# FreeBSD base contains files differing only by case, and the build host
# uses a case-insensitive APFS volume. Keep the stage on a case-sensitive
# sparse image attached inside the project.
DMG=$BUILD/stage.sparseimage
STAGE=$BUILD/stage
need_stage=1
if [ -f "$DMG" ]; then
    hdiutil attach -nobrowse -owners off -mountpoint "$STAGE" "$DMG" >/dev/null 2>&1 || true
    if mount | grep -q " on $ROOT/$STAGE " \
       && [ "$(sed -n '1p' "$STAGE/.staged-variant" 2>/dev/null)" = "$VARIANT" ] \
       && [ "$(sed -n '2p' "$STAGE/.staged-variant" 2>/dev/null)" = "$CONTENT_HASH" ]; then
        need_stage=0
    fi
fi
detach_stage() { hdiutil detach "$STAGE" -force >/dev/null 2>&1 || true; }
trap detach_stage EXIT

if [ "$need_stage" = 1 ]; then
    if mount | grep -q " on $ROOT/$STAGE "; then
        msg "discard stale stage image"
        hdiutil detach "$STAGE" -force >/dev/null
    fi
    rm -f "$DMG"
    rm -rf "$STAGE"
    case "$VARIANT" in
        minimal) stage_size=8g ;;
        desktop) stage_size=14g ;;
    esac
    msg "create case-sensitive sparse image ($DMG, $stage_size)"
    hdiutil create -type SPARSE -fs 'Case-sensitive APFS' -size "$stage_size" \
        -volname E1STAGE "$DMG" >/dev/null
    hdiutil attach -nobrowse -owners off -mountpoint "$STAGE" "$DMG" >/dev/null

    msg "stage: assemble $VARIANT rootfs"
    tar --no-same-owner -xf "$DL/base.txz" -C "$STAGE"
    tar --no-same-owner -xf "$DL/kernel.txz" -C "$STAGE"

    # merge brand overlay over stock FreeBSD
    (cd rootfs && tar cf - .) | (cd "$STAGE" && tar xf -)
    if [ "$VARIANT" = desktop ]; then
        # Extract the closure payloads directly into the stage (usr/local,
        # Gershwin /System), then overlay desktop config on top.
        [ -s "$BUILD/packages/pkg-list.txt" ] \
            || die "desktop closure missing; run: python3 tools/fetch_pkgs.py --out build/packages xorg lightdm lightdm-gtk-greeter mate xfce gershwin xf86-video-scfb xf86-input-libinput"
        msg "stage: extract desktop package payloads"
        python3 tools/build_desktop_blob.py --out "$STAGE" --no-marker \
            | grep -v 'Removing leading' \
            || die "desktop payload extraction failed"
        (cd rootfs-desktop && tar cf - .) | (cd "$STAGE" && tar xf -)
    fi
    for f in etc/fstab boot/loader.conf etc/brand.conf; do
        sed -i '' "s/%VARIANT%/$VARIANT/g; s/%VOLLABEL%/$VOLLABEL/g" "$STAGE/$f"
    done
    # our /etc/motd is custom; login appends the stock template otherwise
    rm -f "$STAGE/etc/motd.template"

    # ship the local e1pkg repository on the media
    mkdir -p "$STAGE/srv/repo"
    cp "$REPO/repo.db" "$REPO"/packages/*.e1.tar.gz "$STAGE/srv/repo/"

    # distribution sets for offline installation
    mkdir -p "$STAGE/usr/freebsd-dist"
    cp "$DL/base.txz" "$DL/kernel.txz" "$STAGE/usr/freebsd-dist/"

    # autosetup media: kernel env baked via loader.conf.d
    if [ -n "$AUTOSETUP" ]; then
        mkdir -p "$STAGE/boot/loader.conf.d"
        cat > "$STAGE/boot/loader.conf.d/autosetup.conf" <<EOF
# e1MinoBSD automatic installation (autosetup media)
minobsd.autosetup="YES"
minobsd.fs="$AUTOSETUP"
EOF
    fi

    printf '%s\n%s\n' "$VARIANT" "$CONTENT_HASH" > "$STAGE/.staged-variant"
fi

if [ "$STAGE_ONLY" = 1 ]; then
    msg "stage complete: $STAGE ($(du -sh "$STAGE" | cut -f1))"
    exit 0
fi

# ------------------------------------------------------------------ iso
MKISOFS=tools/cdrtools/3.02a09/bin/mkisofs
ISO=$BUILD/e1MinoBSD-1.0-$VARIANT-amd64.iso
[ -n "$AUTOSETUP" ] && ISO=$BUILD/e1MinoBSD-1.0-$VARIANT-amd64-auto-$AUTOSETUP.iso
msg "stage: create bootable ISO ($ISO)"
rm -f "$ISO"
"$MKISOFS" -R -J -V "$VOLLABEL" -uid 0 -gid 0 \
    -b boot/cdboot -no-emul-boot -boot-load-size 4 \
    -m .fseventsd -m .staged-variant -m .extract-done \
    -o "$ISO" "$STAGE" 2>&1 | grep -v 'Warning: creating .* transformation table' || true
[ -s "$ISO" ] || die "mkisofs failed"

msg "build complete:"
ls -lh "$ISO" | awk '{printf "    %-12s %s\n", $5, $NF}'
echo  "    test with: ./run-vbox.sh live"
