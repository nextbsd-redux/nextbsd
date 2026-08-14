#!/bin/sh
# qemu-arch.sh — the per-arch qemu shape shared by the two boot harnesses
# (img-boot-test.sh, iso-boot-test.sh). SOURCED, never executed.
#
# Both harnesses boot the same NextBSD rootfs, just wrapped differently (a UFS
# disk image vs. the live ISO), so the only thing that differs between the amd64
# and arm64 lanes is the machine they are booted on:
#
#            amd64                          arm64
#   qemu     qemu-system-x86_64             qemu-system-aarch64
#   machine  q35                            virt
#   firmware OVMF                           AAVMF / QEMU_EFI
#   NIC      e1000 (ROM ships with qemu)    virtio-net-pci, romfile= (see below)
#   TCG cpu  qemu64                         max
#   CD       -cdrom (SATA/ATAPI on q35)     virtio-scsi + scsi-cd (virt has no IDE)
#
# The same split is already proven in nextbsd-userland's tests/boot-test.sh,
# which has been booting the aarch64 CI image under KVM on GitHub's
# ubuntu-24.04-arm runners since #355.
#
# Usage:  . tests/qemu-arch.sh ; qemu_arch_setup <media-path> [<name-hint>]
# Exports (consumed by the generated expect scripts via env):
#   ARCH QEMU MACHINE FW ACCEL_FLAGS NET_ARGS DISK_ARGS CD_ARGS

# qemu_arch_setup <media> [<name-hint>] — resolve the arch and export the qemu
# flags for it. ARCH may be set in the environment (CI passes the matrix arch);
# otherwise it is inferred from the NextBSD-<arch>-<date> name, defaulting to
# amd64 for a hand-named image. <name-hint> is the ORIGINAL artifact name when
# <media> is a decompressed scratch copy (tests/disk.img, tests/live.iso) whose
# name no longer carries the arch.
qemu_arch_setup() {
    _media=$1
    _hint=${2:-$1}

    if [ -z "${ARCH:-}" ]; then
        case "$_hint" in
        *-arm64-*|*-aarch64-*) ARCH=arm64 ;;
        *)                     ARCH=amd64 ;;
        esac
        echo "==> ARCH not set; inferred $ARCH from $(basename "$_hint")"
    fi

    case "$ARCH" in
    amd64)
        QEMU=qemu-system-x86_64
        MACHINE=q35
        NET_ARGS="-nic user,model=e1000"
        _tcg_cpu=qemu64
        _fw_candidates="/usr/share/OVMF/OVMF_CODE.fd /usr/share/ovmf/OVMF.fd /usr/share/qemu/OVMF.fd"
        # q35 has an AHCI controller, so plain -cdrom gets a real ATAPI cd0.
        CD_ARGS="-cdrom $_media -boot d"
        ;;
    arm64|aarch64)
        ARCH=arm64
        QEMU=qemu-system-aarch64
        MACHINE=virt
        # virtio-net-pci defaults to loading a PXE option ROM (efi-virtio.rom)
        # that the arm64 qemu package doesn't ship -> qemu exits "failed to find
        # romfile". We never netboot, so disable it with romfile= (empty); -nic
        # can't express that, hence the -netdev/-device pair.
        NET_ARGS="-netdev user,id=net0 -device virtio-net-pci,netdev=net0,romfile="
        _tcg_cpu=max
        _fw_candidates="/usr/share/qemu-efi-aarch64/QEMU_EFI.fd /usr/share/AAVMF/AAVMF_CODE.fd /usr/share/edk2/aarch64/QEMU_EFI.fd"
        # `virt` has no IDE/AHCI, so -cdrom would land the ISO on a read-only
        # virtio-blk (a vtbd, not a cd). Attach a real SCSI CD instead: EDK2's
        # VirtioScsiDxe boots the El Torito ESP off it, and the guest gets the
        # /dev/cd0 the live init looks for. bootindex pins it as boot device 0
        # (there is no -boot d equivalent for a UEFI machine).
        CD_ARGS="-drive file=$_media,format=raw,if=none,id=cd0,media=cdrom -device virtio-scsi-pci -device scsi-cd,drive=cd0,bootindex=0"
        ;;
    *)
        echo "ERROR: unsupported ARCH=$ARCH (expected amd64 or arm64)" >&2
        return 1
        ;;
    esac

    # virtio-blk is available on both machines and needs no option ROM.
    DISK_ARGS="-drive file=$_media,format=raw,if=virtio"

    # KVM when the runner is the same arch as the guest (ubuntu-24.04 for amd64,
    # ubuntu-24.04-arm for arm64); single-threaded TCG otherwise.
    if [ -e /dev/kvm ]; then
        sudo chmod 666 /dev/kvm 2>/dev/null || true
    fi
    if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
        ACCEL_FLAGS="-accel kvm -cpu host"
        echo "==> using KVM acceleration"
    else
        ACCEL_FLAGS="-accel tcg,thread=single -cpu $_tcg_cpu"
        echo "==> using TCG (single-thread, cpu=$_tcg_cpu)"
    fi

    FW=""
    for _f in $_fw_candidates; do
        [ -f "$_f" ] && { FW="$_f"; break; }
    done
    if [ -z "$FW" ]; then
        echo "ERROR: no UEFI firmware for $ARCH (looked in: $_fw_candidates)" >&2
        return 1
    fi

    echo "==> $ARCH: $QEMU -machine $MACHINE | firmware $FW"
    export ARCH QEMU MACHINE FW ACCEL_FLAGS NET_ARGS DISK_ARGS CD_ARGS
}
