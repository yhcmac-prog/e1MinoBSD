#!/bin/sh
# run-vbox.sh — boot e1MinoBSD ISO in VirtualBox
# Usage: ./run-vbox.sh live     diskless live boot (default)
#        ./run-vbox.sh disk     attach 4GB AHCI disk (installer tests)
#        ./run-vbox.sh clean    unregister and delete the test VM
# Env:   E1BSD_ISO=<path> to boot another image; E1BSD_HEADLESS=1 for automation
set -eu
cd "$(dirname "$0")"

ISO=${E1BSD_ISO:-$PWD/build/e1MinoBSD-1.0-minimal-amd64.iso}
VM=e1MinoBSD
VB=VBoxManage

# Desktop media: larger RAM/disk, USB tablet for evdev pointer input.
DESK=0
case "$ISO" in *-desktop-*) DESK=1 ;; esac
if [ "$DESK" = 1 ]; then
    HD="$PWD/.vbox/e1desktop-test.vdi"
    MEM=1024
    DISKSIZE=12288
else
    HD="$PWD/.vbox/e1minobsd-test.vdi"
    MEM=512
    DISKSIZE=4096
fi

command -v $VB >/dev/null 2>&1 || { echo "ERROR: VirtualBox not installed"; exit 1; }
[ -f "$ISO" ] || { echo "ERROR: $ISO not found, run ./build.sh first"; exit 1; }

if [ "${1:-live}" = "clean" ]; then
    $VB controlvm "$VM" poweroff 2>/dev/null || true
    $VB unregistervm "$VM" --delete 2>/dev/null || true
    echo "Cleaned VM: $VM"
    exit 0
fi

WITH_DISK=0
[ "${1:-live}" = "disk" ] && WITH_DISK=1

mkdir -p "$PWD/.vbox"
: > "$PWD/.vbox/serial.log"   # file mode appends; clear before each run

if ! $VB list vms | grep -q "\"$VM\""; then
    $VB createvm --name "$VM" --basefolder "$PWD/.vbox" --register
    sleep 2
    if [ "$DESK" = 1 ]; then
        usbset="--usb on --usbehci on --mouse usbtablet"
    else
        usbset="--usb off"
    fi
    $VB modifyvm "$VM" --ostype FreeBSD_64 --memory "$MEM" \
        --audio none $usbset --graphicscontroller vboxvga --boot1 dvd \
        --nic1 nat --nictype1 82540EM --cableconnected1 on \
        --uart1 0x3F8 4 --uart-mode1 file "$PWD/.vbox/serial.log"
    sleep 2
    $VB storagectl "$VM" --name IDE --add ide --controller PIIX4
    sleep 2
    $VB storagectl "$VM" --name AHCI --add sata
    sleep 2
    if [ "$WITH_DISK" = 1 ]; then
        $VB createmedium disk --filename "$HD" --size "$DISKSIZE" --format VDI
        $VB storageattach "$VM" --storagectl AHCI --port 0 --device 0 \
            --type hdd --medium "$HD"
    fi
elif [ "$WITH_DISK" = 1 ]; then
    if [ ! -f "$HD" ]; then
        $VB storageattach "$VM" --storagectl AHCI --port 0 --device 0 \
            --type hdd --medium none 2>/dev/null || true
        $VB closemedium disk "$HD" 2>/dev/null || true
        $VB createmedium disk --filename "$HD" --size "$DISKSIZE" --format VDI
    fi
    [ "$DESK" = 1 ] && $VB modifyvm "$VM" --memory "$MEM" \
        --usb on --usbehci on --mouse usbtablet
    $VB storageattach "$VM" --storagectl AHCI --port 0 --device 0 \
        --type hdd --medium "$HD"
fi

# always refresh the DVD attachment (rebuilt ISO / variant switch)
$VB storageattach "$VM" --storagectl IDE --port 1 --device 0 \
    --type dvddrive --medium "$ISO"

if [ "${E1BSD_HEADLESS:-}" = "1" ]; then
    $VB startvm "$VM" --type headless
else
    open -na /Applications/VirtualBox.app/Contents/Resources/VirtualBoxVM.app \
        --args --comment "$VM" --startvm "$($VB list vms | grep "\"$VM\"" | sed 's/.*{\(.*\)}/\1/')"
fi
echo "VM started. Serial log: .vbox/serial.log"
