#!/usr/bin/env bash
#
# setup-linux.sh - check and install what mstar-flasher needs to talk to an
# FTDI FT2232C/D programmer (or an H-series MPSSE adapter) on Linux.
#
# What it looks at:
#   1. development files for libftdi1 and libusb-1.0 (and pkg-config)
#   2. a udev rule that lets members of "plugdev" open the FTDI device without
#      root, and tells ModemManager to leave it alone
#   3. membership of the target user in "plugdev"
#   4. (informational) attached FTDI devices, build tools, kernel driver
#
# It never changes anything without showing a plan first and asking, and it is
# safe to run again: whatever is already in place is left alone.
#
# Usage:
#   ./scripts/setup-linux.sh --check            report only, no root needed
#   sudo ./scripts/setup-linux.sh               show the plan, ask, apply
#   sudo ./scripts/setup-linux.sh --yes         same, without the question
#
# See --help for all options. Package installation is supported on
# Debian/Ubuntu (apt); elsewhere the script checks and tells you what to install.

set -euo pipefail

RULES_NAME="60-mstar-ftdi.rules"
UDEV_RULES_DIR="${UDEV_RULES_DIR:-/etc/udev/rules.d}"   # override is for testing only
FTDI_VID="0403"
GROUP_NAME="plugdev"
PIDS=("6010" "6011" "6014")  # FT2232C/D/H, FT4232H, FT232H
DEV_PACKAGES=(libftdi1-dev libusb-1.0-0-dev pkg-config)
MIN_CMAKE="3.28"
MIN_GCC="13"
MIN_LIBFTDI="1.5"
# Read-only overrides for deterministic tests; normal runs inspect the real host.
USB_SYSFS_DIR="${USB_SYSFS_DIR:-/sys/bus/usb/devices}"
USB_DEVICE_DIR="${USB_DEVICE_DIR:-/dev/bus/usb}"

CHECK_ONLY=0
ASSUME_YES=0
SKIP_PACKAGES=0
SKIP_UDEV=0
WITH_BUILD_TOOLS=0
TARGET_USER=""

HAVE_APT=0
ACTIONS=()          # human-readable plan: what this script will do
LEFT=()             # missing things this script will NOT fix (skipped, or no apt)
APT_PKGS=()         # packages to install
NEED_RULE=0
NEED_GROUP=0
NEED_GROUP_CREATE=0

if [[ -t 1 ]]; then
    C_OK=$'\033[32m'; C_WARN=$'\033[33m'; C_BAD=$'\033[31m'; C_BOLD=$'\033[1m'; C_OFF=$'\033[0m'
else
    C_OK=""; C_WARN=""; C_BAD=""; C_BOLD=""; C_OFF=""
fi

ok()      { printf '  %s[ OK ]%s %s\n' "$C_OK" "$C_OFF" "$*"; }
warn()    { printf '  %s[WARN]%s %s\n' "$C_WARN" "$C_OFF" "$*"; }
bad()     { printf '  %s[ !! ]%s %s\n' "$C_BAD" "$C_OFF" "$*"; }
info()    { printf '         %s\n' "$*"; }
section() { printf '\n%s%s%s\n' "$C_BOLD" "$*" "$C_OFF"; }
die()     { printf '%serror:%s %s\n' "$C_BAD" "$C_OFF" "$*" >&2; exit 2; }

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Checks (and with root, installs) what mstar-flasher needs for an FTDI
FT2232C/D or H-series interface on Linux. Always shows a plan before changing anything.

Options:
  --check             Report only; change nothing, no root needed.
                      Exit status 0 = nothing to do, 1 = something is missing.
  -y, --yes           Do not ask for confirmation.
  --user NAME         User to put in group '$GROUP_NAME' (default: the user who ran sudo).
  --pid HEX           Also grant access to FTDI USB product id HEX (4 hex digits,
                      for a custom adapter). Repeatable. Default: ${PIDS[*]}.
                      This only grants USB access; it does not add backend device support.
  --skip-packages     Do not install packages (still reports them).
  --skip-udev         Do not touch the udev rule (still reports it).
  --with-build-tools  Also install missing build tools (build-essential, cmake,
                      ninja-build, git, curl, zip, unzip). Note: the distro's cmake/g++ may be older
                      than this project needs (cmake >= $MIN_CMAKE, g++ >= $MIN_GCC).
  -h, --help          This text.

Examples:
  ./scripts/setup-linux.sh --check
  sudo ./scripts/setup-linux.sh
  sudo ./scripts/setup-linux.sh --yes --with-build-tools
EOF
}

# ------------------------------------------------------------------ args ---

while (($#)); do
    case "$1" in
        --check)            CHECK_ONLY=1 ;;
        -y|--yes)           ASSUME_YES=1 ;;
        --user)             [[ $# -ge 2 ]] || die "--user needs a name"; TARGET_USER="$2"; shift ;;
        --pid)
            [[ $# -ge 2 ]] || die "--pid needs a value"
            [[ $2 =~ ^[0-9a-fA-F]{4}$ ]] || die "--pid expects 4 hex digits, got '$2'"
            pid_lc=$(printf '%s' "$2" | tr 'A-F' 'a-f')
            already=0
            for p in "${PIDS[@]}"; do [[ $p == "$pid_lc" ]] && already=1; done
            if ((already == 0)); then PIDS+=("$pid_lc"); fi
            shift ;;
        --skip-packages)    SKIP_PACKAGES=1 ;;
        --skip-udev)        SKIP_UDEV=1 ;;
        --with-build-tools) WITH_BUILD_TOOLS=1 ;;
        -h|--help)          usage; exit 0 ;;
        *)                  printf 'unknown option: %s\n\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

# ----------------------------------------------------------------- helpers ---

add_action() { ACTIONS+=("$*"); }
add_left()   { LEFT+=("$*"); }

pkg_installed() {
    dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -q '^install ok installed$'
}

# version_ge A B  ->  true if A >= B
version_ge() {
    [[ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n1)" == "$2" ]]
}

resolve_user() {
    if [[ -z $TARGET_USER ]]; then
        if [[ -n ${SUDO_USER:-} && $SUDO_USER != root ]]; then
            TARGET_USER=$SUDO_USER
        elif ((EUID != 0)); then
            TARGET_USER=$(id -un)
        fi
    fi
    if [[ -n $TARGET_USER ]] && ! id "$TARGET_USER" >/dev/null 2>&1; then
        die "user '$TARGET_USER' does not exist"
    fi
}

expected_rules() {
    local pid
    cat <<EOF
# Installed by mstar-flasher scripts/setup-linux.sh - re-run the script to change it.
# 1) Lets members of group $GROUP_NAME (and the logged-in desktop user) open the
#    FTDI USB device through libusb/libftdi1 without root.
# 2) Tells ModemManager not to probe the device as a modem: it would send AT
#    commands to the serial port that appears while the kernel driver is bound.
# Only the USB device node is touched; /dev/ttyUSB* keeps its normal group.
EOF
    for pid in "${PIDS[@]}"; do
        printf 'SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTR{idVendor}=="%s", ATTR{idProduct}=="%s", MODE="0660", GROUP="%s", TAG+="uaccess"\n' \
            "$FTDI_VID" "$pid" "$GROUP_NAME"
        printf 'ATTRS{idVendor}=="%s", ATTRS{idProduct}=="%s", ENV{ID_MM_DEVICE_IGNORE}="1"\n' \
            "$FTDI_VID" "$pid"
    done
}

# --------------------------------------------------------------- the checks ---

check_os() {
    section "System"
    local name="unknown"
    if [[ -r /etc/os-release ]]; then
        name=$(. /etc/os-release && printf '%s' "${PRETTY_NAME:-unknown}")
    fi
    if command -v apt-get >/dev/null 2>&1 && command -v dpkg-query >/dev/null 2>&1; then
        HAVE_APT=1
        ok "$name (apt available: packages can be installed by this script)"
    else
        warn "$name (no apt: this script can check but not install packages)"
    fi
}

check_dev_libs() {
    section "Development libraries (libftdi1, libusb-1.0)"
    local all_ok=1 mod p
    local missing=()
    if command -v pkg-config >/dev/null 2>&1; then
        for mod in libftdi1 libusb-1.0; do
            if pkg-config --exists "$mod"; then
                if [[ $mod == libftdi1 ]] && ! pkg-config --atleast-version="$MIN_LIBFTDI" "$mod"; then
                    bad "$mod $(pkg-config --modversion "$mod"): version $MIN_LIBFTDI or newer is required"
                    add_left "upgrade libftdi1 development files to version $MIN_LIBFTDI or newer"
                else
                    ok "$mod $(pkg-config --modversion "$mod")"
                fi
            else
                bad "$mod: development files not found by pkg-config"
                all_ok=0
            fi
        done
    else
        bad "pkg-config is not installed"
        all_ok=0
    fi
    ((all_ok == 0)) || return 0

    if ((HAVE_APT == 0)); then
        info "Install them with your package manager, typical names:"
        info "  Fedora: libftdi-devel libusb1-devel pkgconf-pkg-config"
        info "  Arch:   libftdi libusb pkgconf"
        add_left "install libftdi1 and libusb-1.0 development files (no apt here)"
        return 0
    fi
    for p in "${DEV_PACKAGES[@]}"; do
        pkg_installed "$p" || missing+=("$p")
    done
    if ((${#missing[@]} == 0)); then
        warn "all of ${DEV_PACKAGES[*]} are installed but pkg-config cannot see them (check PKG_CONFIG_PATH)"
        add_left "make pkg-config find libftdi1 / libusb-1.0"
    elif ((SKIP_PACKAGES)); then
        info "would install: ${missing[*]} (skipped: --skip-packages)"
        add_left "install ${missing[*]}"
    else
        APT_PKGS+=("${missing[@]}")
        add_action "apt-get install ${missing[*]}"
    fi
}

check_build_tools() {
    section "Build tools (informational)"
    local v major tool missing_pkgs=()

    if command -v cmake >/dev/null 2>&1; then
        v=$(cmake --version | head -n1 | awk '{print $3}')
        if version_ge "$v" "$MIN_CMAKE"; then ok "cmake $v"
        else warn "cmake $v is older than the required $MIN_CMAKE (apt cannot fix this on this release)"; fi
    else
        warn "cmake not found"; missing_pkgs+=(cmake)
    fi

    if command -v ninja >/dev/null 2>&1; then ok "ninja $(ninja --version)"
    else warn "ninja not found (the CMake presets use it)"; missing_pkgs+=(ninja-build); fi

    if command -v g++ >/dev/null 2>&1; then
        v=$(g++ -dumpfullversion 2>/dev/null || g++ -dumpversion)
        major=${v%%.*}
        if ((major >= MIN_GCC)); then ok "g++ $v"
        else warn "g++ $v: version $MIN_GCC or newer (or another C++23 compiler) is needed"; fi
    elif command -v clang++ >/dev/null 2>&1; then
        warn "clang++ found but not verified for this project; g++ >= $MIN_GCC is the tested compiler"
    else
        warn "no C++ compiler found"; missing_pkgs+=(build-essential)
    fi

    if command -v git >/dev/null 2>&1; then ok "git $(git --version | awk '{print $3}')"
    else warn "git not found"; missing_pkgs+=(git); fi

    for tool in curl zip unzip; do
        if command -v "$tool" >/dev/null 2>&1; then ok "$tool (vcpkg prerequisite)"
        else warn "$tool not found (needed by vcpkg)"; missing_pkgs+=("$tool"); fi
    done

    if [[ -z ${VCPKG_ROOT:-} ]]; then
        warn "VCPKG_ROOT is not set; the CMake presets require an existing vcpkg checkout"
    elif [[ ! -f $VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake || ! -x $VCPKG_ROOT/vcpkg ]]; then
        warn "VCPKG_ROOT=$VCPKG_ROOT is not a bootstrapped vcpkg checkout"
    else
        ok "vcpkg at $VCPKG_ROOT"
    fi
    info "This script does not install vcpkg; export VCPKG_ROOT before configuring."

    if ((${#missing_pkgs[@]} == 0)); then return 0; fi
    if ((WITH_BUILD_TOOLS && HAVE_APT && SKIP_PACKAGES == 0)); then
        APT_PKGS+=("${missing_pkgs[@]}")
        add_action "apt-get install ${missing_pkgs[*]}   (--with-build-tools)"
    else
        info "to install them with this script: add --with-build-tools (apt only)"
    fi
}

check_udev_rule() {
    section "udev rule (USB access without root)"
    local file="$UDEV_RULES_DIR/$RULES_NAME"
    if [[ -f $file ]]; then
        if [[ "$(cat "$file")" == "$(expected_rules)" ]]; then
            ok "$file is up to date (FTDI product ids: ${PIDS[*]})"
            return 0
        fi
        bad "$file exists but differs from what this script would write"
    else
        bad "$file is missing"
    fi
    if ((SKIP_UDEV)); then
        info "skipped: --skip-udev"
        add_left "install the udev rule $file"
    else
        NEED_RULE=1
        add_action "write $file (FTDI ${FTDI_VID}:{${PIDS[*]}}) and reload udev"
    fi
}

check_group() {
    section "Group membership"
    if ! getent group "$GROUP_NAME" >/dev/null 2>&1; then
        bad "group '$GROUP_NAME' does not exist on this system"
        NEED_GROUP_CREATE=1
        add_action "groupadd --system $GROUP_NAME"
    fi
    if [[ -z $TARGET_USER ]]; then
        warn "no target user known (run through sudo, or pass --user NAME)"
        return 0
    fi
    if id -nG "$TARGET_USER" | tr ' ' '\n' | grep -qx "$GROUP_NAME"; then
        ok "$TARGET_USER is in group $GROUP_NAME"
    else
        bad "$TARGET_USER is not in group $GROUP_NAME"
        NEED_GROUP=1
        add_action "usermod -aG $GROUP_NAME $TARGET_USER   (takes effect at next login)"
    fi
}

can_access() {   # can_access NODE -> does the target user get read+write on it?
    local node=$1
    if [[ -n $TARGET_USER && $EUID -eq 0 ]] && command -v runuser >/dev/null 2>&1; then
        runuser -u "$TARGET_USER" -- test -r "$node" -a -w "$node"
    else
        [[ -r $node && -w $node ]]
    fi
}

check_device_node() {
    local node=$1
    if [[ ! -e $node ]]; then
        bad "USB device node $node is missing (disconnected device or USB not exposed here)"
        add_left "make USB device node $node visible in this environment, then re-run --check"
    elif can_access "$node"; then
        ok "device node $node is accessible${TARGET_USER:+ to $TARGET_USER}"
    else
        bad "USB device node $node exists but is not readable/writable${TARGET_USER:+ by $TARGET_USER}"
        add_left "check udev permissions and active login groups for $node; re-plug/re-login if needed"
    fi
}

scan_devices() {
    section "Attached FTDI devices (USB vendor $FTDI_VID)"
    local d p pid prod ser mfr covered drv iface node found=0
    for d in "$USB_SYSFS_DIR"/*; do
        [[ -r $d/idVendor ]] || continue
        [[ "$(cat "$d/idVendor")" == "$FTDI_VID" ]] || continue
        found=1
        pid=$(cat "$d/idProduct" 2>/dev/null || echo "????")
        prod=$(cat "$d/product" 2>/dev/null || echo "?")
        mfr=$(cat "$d/manufacturer" 2>/dev/null || echo "?")
        ser=$(cat "$d/serial" 2>/dev/null || echo "-")
        ok "${FTDI_VID}:${pid}  $mfr / $prod  (serial $ser)"

        covered=0
        for p in "${PIDS[@]}"; do [[ $p == "$pid" ]] && covered=1; done
        if ((covered == 0)); then
            warn "product id $pid is not covered by the udev rule; re-run with: --pid $pid"
        fi

        drv=""
        for iface in "$d":*; do
            if [[ -L $iface/driver ]]; then drv="$(basename "$(readlink "$iface/driver")")"; fi
        done
        if [[ -n $drv ]]; then
            info "kernel driver bound: $drv (libftdi1 normally detaches it when opening the device)"
        fi

        if [[ -r $d/busnum && -r $d/devnum ]]; then
            node=$(printf '%s/%03d/%03d' "$USB_DEVICE_DIR" "$(cat "$d/busnum")" "$(cat "$d/devnum")")
            check_device_node "$node"
        fi
    done
    if ((found == 0)); then
        info "none attached. Plug the interface board in and run:  $(basename "$0") --check"
    fi
}

check_modemmanager() {
    if command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet ModemManager 2>/dev/null; then
        section "ModemManager"
        info "ModemManager is running; the udev rule above makes it ignore the listed FTDI ids"
        info "(otherwise it may probe the serial port, which could disturb the I2C lines)."
    fi
}

run_checks() {
    ACTIONS=(); LEFT=(); APT_PKGS=(); NEED_RULE=0; NEED_GROUP=0; NEED_GROUP_CREATE=0
    check_os
    check_dev_libs
    check_build_tools
    check_udev_rule
    check_group
    scan_devices
    check_modemmanager
}

# ------------------------------------------------------------------- apply ---

apply_changes() {
    if ((${#APT_PKGS[@]} > 0)); then
        section "Installing packages: ${APT_PKGS[*]}"
        export DEBIAN_FRONTEND=noninteractive
        apt-get update || warn "apt-get update failed; trying with the existing package lists"
        apt-get install -y --no-install-recommends "${APT_PKGS[@]}"
    fi

    if ((NEED_GROUP_CREATE)); then
        section "Creating group $GROUP_NAME"
        groupadd --system "$GROUP_NAME"
    fi

    if ((NEED_RULE)); then
        section "Writing the udev rule"
        local file="$UDEV_RULES_DIR/$RULES_NAME" tmp
        mkdir -p "$UDEV_RULES_DIR"
        if [[ -f $file ]]; then
            cp -p "$file" "$file.bak.$(date +%Y%m%d%H%M%S)"
            info "previous file saved as $file.bak.*"
        fi
        tmp=$(mktemp "$UDEV_RULES_DIR/.${RULES_NAME}.XXXXXX")
        expected_rules > "$tmp"
        chmod 0644 "$tmp"
        mv "$tmp" "$file"
        ok "wrote $file"
        if command -v udevadm >/dev/null 2>&1; then
            if udevadm control --reload-rules \
               && udevadm trigger --subsystem-match=usb --attr-match=idVendor="$FTDI_VID"; then
                ok "udev rules reloaded and applied to attached devices"
            else
                warn "could not reload udev; run: udevadm control --reload-rules && udevadm trigger"
            fi
        else
            warn "udevadm not found; reboot or re-plug the device to apply the rule"
        fi
    fi

    if ((NEED_GROUP)); then
        section "Adding $TARGET_USER to group $GROUP_NAME"
        usermod -aG "$GROUP_NAME" "$TARGET_USER"
        ok "done - $TARGET_USER must log out and back in for it to take effect"
    fi
}

confirm() {
    if ((ASSUME_YES)); then return 0; fi
    if [[ ! -t 0 ]]; then die "no terminal to ask on; pass --yes to proceed"; fi
    local answer
    read -r -p "Proceed? [y/N] " answer
    [[ $answer == [yY]* ]]
}

# -------------------------------------------------------------------- main ---

main() {
    local a rc group_changed

    resolve_user
    printf '%smstar-flasher: Linux FTDI setup%s\n' "$C_BOLD" "$C_OFF"
    run_checks

    section "Plan"
    if ((${#ACTIONS[@]} == 0 && ${#LEFT[@]} == 0)); then
        ok "nothing to do - everything this script checks is in place"
        exit 0
    fi
    for a in "${ACTIONS[@]}"; do info "- $a"; done
    if ((${#LEFT[@]} > 0)); then
        warn "this script will not do the following (do it yourself):"
        for a in "${LEFT[@]}"; do info "- $a"; done
    fi

    if ((${#ACTIONS[@]} == 0)); then
        exit 1      # only manual items remain; nothing for the script to apply
    fi
    if ((CHECK_ONLY)); then
        printf '\nRun without --check, as root, to apply:  sudo %s\n' "$0"
        exit 1
    fi
    if ((EUID != 0)); then
        printf '\n'
        die "changes need root. Run:  sudo $0   (or use --check to only report)"
    fi

    printf '\n'
    if ! confirm; then
        echo "Aborted; nothing was changed."
        exit 1
    fi

    group_changed=$NEED_GROUP
    apply_changes

    printf '\n%sRe-checking...%s\n' "$C_BOLD" "$C_OFF"
    run_checks
    section "Result"
    rc=0
    if ((${#ACTIONS[@]} > 0)); then
        bad "still pending:"
        for a in "${ACTIONS[@]}"; do info "- $a"; done
        rc=1
    fi
    if ((${#LEFT[@]} > 0)); then
        warn "left for you to do:"
        for a in "${LEFT[@]}"; do info "- $a"; done
        rc=1
    fi
    if ((rc == 0)); then
        ok "everything is in place"
    fi

    section "Next"
    info "- Re-plug the interface board so the new rule is applied to it."
    if ((group_changed)); then
        info "- Log out and back in ($TARGET_USER was added to group $GROUP_NAME)."
    fi
    info "- Verify with:  $(basename "$0") --check   (as the normal user)"
    exit "$rc"
}

# Run only when executed, not when sourced (lets a test harness call the functions).
if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    main "$@"
fi
