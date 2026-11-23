#!/usr/bin/env bash
# Only temporary files and sourced check functions; never sudo or real setup.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../scripts/setup-linux.sh"
setup_test_dir=$(mktemp -d)
trap 'rm -rf "$setup_test_dir"' EXIT

# Rules cover every standard product ID supported by this project's backend.
expected_rules > "$setup_test_dir/rules"
for pid in 6010 6011 6014; do
    grep -q "ATTR{idProduct}==\"$pid\"" "$setup_test_dir/rules"
    grep -q "ATTRS{idProduct}==\"$pid\".*ID_MM_DEVICE_IGNORE" "$setup_test_dir/rules"
done
grep -q 'MODE="0660", GROUP="plugdev", TAG+="uaccess"' "$setup_test_dir/rules"
if grep -q '0666' "$setup_test_dir/rules"; then exit 1; fi

# Check/apply/re-check against temporary rules, with all host mutations mocked.
(
    UDEV_RULES_DIR="$setup_test_dir/udev"
    ACTIONS=(); LEFT=(); NEED_RULE=0; NEED_GROUP=0; NEED_GROUP_CREATE=0; APT_PKGS=()
    check_udev_rule > "$setup_test_dir/output"
    [[ $NEED_RULE == 1 && ! -e $UDEV_RULES_DIR/$RULES_NAME ]]
    udevadm() { printf '%s\n' "$*" >> "$setup_test_dir/udev-calls"; }
    apt-get() { echo 'unexpected package installation' >&2; exit 1; }
    usermod() { echo 'unexpected user change' >&2; exit 1; }
    groupadd() { echo 'unexpected group creation' >&2; exit 1; }
    apply_changes > "$setup_test_dir/output"
    cmp "$setup_test_dir/rules" "$UDEV_RULES_DIR/$RULES_NAME"
    grep -q 'control --reload-rules' "$setup_test_dir/udev-calls"
    grep -q 'trigger --subsystem-match=usb --attr-match=idVendor=0403' "$setup_test_dir/udev-calls"
    ACTIONS=(); NEED_RULE=0
    check_udev_rule > "$setup_test_dir/output"
    [[ $NEED_RULE == 0 && ${#ACTIONS[@]} == 0 ]]
    echo '# custom previous rule' > "$UDEV_RULES_DIR/$RULES_NAME"
    SKIP_UDEV=1; LEFT=()
    check_udev_rule > "$setup_test_dir/output"
    [[ $NEED_RULE == 0 && ${#LEFT[@]} == 1 ]]
    SKIP_UDEV=0
    check_udev_rule > "$setup_test_dir/output"
    apply_changes > "$setup_test_dir/output"
    cmp "$setup_test_dir/rules" "$UDEV_RULES_DIR/$RULES_NAME"
    backups=("$UDEV_RULES_DIR/$RULES_NAME".bak.*)
    [[ ${#backups[@]} == 1 ]]
    grep -q 'custom previous rule' "${backups[0]}"
)

# A missing group is planned explicitly, including membership for the target user.
(
    getent() { return 2; }
    id() { echo 'tester users'; }
    TARGET_USER=tester
    ACTIONS=(); NEED_GROUP=0; NEED_GROUP_CREATE=0
    check_group > "$setup_test_dir/output"
    [[ $NEED_GROUP_CREATE == 1 && $NEED_GROUP == 1 && ${#ACTIONS[@]} == 2 ]]
    groupadd() { echo "groupadd $*" >> "$setup_test_dir/group-calls"; }
    usermod() { echo "usermod $*" >> "$setup_test_dir/group-calls"; }
    NEED_RULE=0; APT_PKGS=()
    apply_changes > "$setup_test_dir/output"
    grep -q '^groupadd --system plugdev$' "$setup_test_dir/group-calls"
    grep -q '^usermod -aG plugdev tester$' "$setup_test_dir/group-calls"
)

# Sysfs inventory is inspected without opening or claiming any USB interface.
(
    USB_SYSFS_DIR="$setup_test_dir/sysfs"
    USB_DEVICE_DIR="$setup_test_dir/usb"
    mkdir -p "$USB_SYSFS_DIR/1-2" "$USB_DEVICE_DIR/001"
    printf '0403\n' > "$USB_SYSFS_DIR/1-2/idVendor"
    printf '6010\n' > "$USB_SYSFS_DIR/1-2/idProduct"
    printf 'FT2232D\n' > "$USB_SYSFS_DIR/1-2/product"
    printf 'FTDI\n' > "$USB_SYSFS_DIR/1-2/manufacturer"
    printf 'MSTAR\n' > "$USB_SYSFS_DIR/1-2/serial"
    printf '1\n' > "$USB_SYSFS_DIR/1-2/busnum"
    printf '5\n' > "$USB_SYSFS_DIR/1-2/devnum"
    touch "$USB_DEVICE_DIR/001/005"
    can_access() { return 0; }
    LEFT=()
    scan_devices > "$setup_test_dir/output"
    grep -q '0403:6010.*FT2232D.*MSTAR' "$setup_test_dir/output"
    grep -q '001/005 is accessible' "$setup_test_dir/output"
    [[ ${#LEFT[@]} == 0 ]]
)

# --check must never call apply, even when --yes and pending actions are present.
(
    resolve_user() { :; }
    run_checks() { ACTIONS=("test action"); LEFT=(); }
    apply_changes() { touch "$setup_test_dir/unexpected-apply"; }
    CHECK_ONLY=1; ASSUME_YES=1
    rc=0
    (main) > "$setup_test_dir/output" || rc=$?
    [[ $rc == 1 && ! -e $setup_test_dir/unexpected-apply ]]
)

LEFT=()
check_device_node "$setup_test_dir/missing" > "$setup_test_dir/output"
[[ ${#LEFT[@]} == 1 ]]
grep -q 'missing' "$setup_test_dir/output"
grep -q 'not exposed' "$setup_test_dir/output"

touch "$setup_test_dir/device"
# Deterministic even for a root test runner; permission logic itself uses test -r/-w.
can_access() { return 1; }
LEFT=()
check_device_node "$setup_test_dir/device" > "$setup_test_dir/output"
[[ ${#LEFT[@]} == 1 ]]
grep -q 'exists but' "$setup_test_dir/output"

can_access() { return 0; }
LEFT=()
check_device_node "$setup_test_dir/device" > "$setup_test_dir/output"
[[ ${#LEFT[@]} == 0 ]]
grep -q 'accessible' "$setup_test_dir/output"

# Exercise main's real readiness exit code with isolated checks and no mutations.
resolve_user() { :; }
run_checks() { ACTIONS=(); LEFT=(); check_device_node "$setup_test_dir/missing"; }
CHECK_ONLY=1
if (main) > "$setup_test_dir/output"; then
    echo 'missing USB node incorrectly reported success' >&2
    exit 1
fi
if grep -q 'everything.*in place' "$setup_test_dir/output"; then exit 1; fi
run_checks() { ACTIONS=(); LEFT=(); check_device_node "$setup_test_dir/device"; }
(main) > "$setup_test_dir/output"
grep -q 'everything.*in place' "$setup_test_dir/output"

# An installed but too-old SDK must not pass the prerequisite check.
pkg-config() {
    case "$1" in
        --exists) return 0 ;;
        --modversion) echo 1.4 ;;
        --atleast-version=*) return 1 ;;
        *) return 1 ;;
    esac
}
LEFT=()
check_dev_libs > "$setup_test_dir/output"
[[ ${#LEFT[@]} == 1 ]]
grep -q '1.5 or newer' "$setup_test_dir/output"
