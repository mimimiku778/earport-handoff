#!/bin/bash
#
# AirPods Seamless Switching for Ubuntu
# Installs, updates, or removes the daemon and GNOME Shell extension.
#

set -euo pipefail

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXTENSION_UUID="earport@anoryth.github.io"
USER_DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
USER_CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
INSTALL_PREFIX="$HOME/.local"
EXTENSIONS_ROOT="$USER_DATA_HOME/gnome-shell/extensions"
EXTENSION_DIR="$EXTENSIONS_ROOT/$EXTENSION_UUID"
LEGACY_EXTENSIONS_ROOT="$INSTALL_PREFIX/share/gnome-shell/extensions"
LEGACY_EXTENSION_DIR="$LEGACY_EXTENSIONS_ROOT/$EXTENSION_UUID"
# The generated systemd and D-Bus launchers rely on standard XDG user search
# paths. Keep the executable under ~/.local/bin while honoring a custom
# XDG_DATA_HOME for every data file that the desktop must discover.
DAEMON_BUILD_DIR="$SCRIPT_DIR/daemon/build"

check_supported_platform() {
    local distro_id=""
    local version_id=""
    local major
    local minor

    if [ -r /etc/os-release ]; then
        distro_id="$(sed -n 's/^ID=//p' /etc/os-release | tr -d '"' | head -n 1)"
        version_id="$(sed -n 's/^VERSION_ID=//p' /etc/os-release | tr -d '"' | head -n 1)"
    fi

    if [ "$distro_id" != "ubuntu" ] ||
       [[ ! "$version_id" =~ ^([0-9]+)\.([0-9]+) ]]; then
        print_error "This installer supports Ubuntu 26.04 or newer only"
        exit 1
    fi

    major="${BASH_REMATCH[1]}"
    minor="${BASH_REMATCH[2]}"
    if (( major < 26 || (major == 26 && minor < 4) )); then
        print_error "Ubuntu $version_id is unsupported; Ubuntu 26.04 or newer is required"
        exit 1
    fi

    print_success "Supported platform: Ubuntu $version_id"
}

remove_owned_directory() {
    local target=$1
    local parent=$2

    # Refuse an empty, root, or parent-wide recursive deletion even if an
    # unusual environment variable or checkout path was supplied.
    if [ -z "$target" ] || [ -z "$parent" ] ||
       [[ "$target" != /* ]] || [[ "$parent" != /* ]] ||
       [ "$target" = "/" ] || [ "$target" = "$parent" ] ||
       [[ "$target" != "$parent/"* ]]; then
        print_error "Refusing unsafe directory removal: $target"
        exit 1
    fi

    rm -rf -- "$target"
}

print_header() {
    echo -e "${BLUE}"
    echo "╔════════════════════════════════════════════╗"
    echo "║        AirPods for Ubuntu Installer        ║"
    echo "╚════════════════════════════════════════════╝"
    echo -e "${NC}"
}

print_step() {
    echo -e "${BLUE}==>${NC} $1"
}

print_success() {
    echo -e "${GREEN}✓${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}⚠${NC} $1"
}

print_error() {
    echo -e "${RED}✗${NC} $1"
}

check_dependencies() {
    print_step "Checking dependencies..."

    local missing_deps=()

    # Check build tools
    if ! command -v meson &> /dev/null; then
        missing_deps+=("meson")
    fi

    if ! command -v ninja &> /dev/null; then
        missing_deps+=("ninja")
    fi

    # Check pkg-config
    if ! command -v pkg-config &> /dev/null; then
        missing_deps+=("pkg-config")
    fi

    # Check for GLib development files
    if ! pkg-config --exists glib-2.0 2>/dev/null; then
        missing_deps+=("libglib2.0-dev")
    fi

    # Check for BlueZ development files
    if ! pkg-config --exists bluez 2>/dev/null; then
        missing_deps+=("libbluetooth-dev")
    fi

    # Check for GNOME Shell
    if ! command -v gnome-shell &> /dev/null; then
        missing_deps+=("gnome-shell")
    fi

    # The asynchronous audio router uses pactl to select the AirPods A2DP sink
    # and move active streams. PipeWire provides it through pipewire-pulse.
    if ! command -v pactl &> /dev/null; then
        missing_deps+=("pulseaudio-utils (pactl)")
    fi

    if [ "${#missing_deps[@]}" -ne 0 ]; then
        print_error "Missing dependencies:"
        for dep in "${missing_deps[@]}"; do
            echo "  - $dep"
        done
        echo ""
        echo "Install them using your package manager:"
        echo ""
        echo "  Ubuntu 26.04 or newer:"
        echo "    sudo apt install meson ninja-build pkg-config libglib2.0-dev libbluetooth-dev gnome-shell pulseaudio-utils"
        echo ""
        exit 1
    fi

    print_success "All dependencies found"
}

build_daemon() {
    print_step "Building daemon..."

    # Clean previous build if exists
    if [ -d "$DAEMON_BUILD_DIR" ]; then
        remove_owned_directory "$DAEMON_BUILD_DIR" "$SCRIPT_DIR/daemon"
    fi

    meson setup "$DAEMON_BUILD_DIR" "$SCRIPT_DIR/daemon" \
        --buildtype=release \
        --prefix="$INSTALL_PREFIX" \
        --datadir="$USER_DATA_HOME"
    ninja -C "$DAEMON_BUILD_DIR"

    print_success "Daemon built successfully"
}

update_checkout() {
    print_step "Checking for updates..."

    local checkout_root
    local script_root

    if ! command -v git &> /dev/null; then
        print_error "Git is required for --update"
        exit 1
    fi

    if ! checkout_root="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel 2>/dev/null)"; then
        print_error "This directory is not a Git checkout"
        echo "    Download a fresh release or clone the repository again."
        exit 1
    fi

    checkout_root="$(cd "$checkout_root" && pwd -P)"
    script_root="$(cd "$SCRIPT_DIR" && pwd -P)"
    if [ "$checkout_root" != "$script_root" ]; then
        print_error "This installer is not at the root of its Git checkout"
        echo "    Refusing to update the parent repository: $checkout_root"
        exit 1
    fi

    if [ -n "$(git -C "$SCRIPT_DIR" status --porcelain --untracked-files=normal)" ]; then
        print_error "The checkout has local changes"
        echo "    Commit, stash, or remove them before updating."
        exit 1
    fi

    if ! git -C "$SCRIPT_DIR" symbolic-ref --quiet HEAD &> /dev/null; then
        print_error "Cannot update a detached Git checkout"
        exit 1
    fi

    git -C "$SCRIPT_DIR" pull --ff-only
    print_success "Repository is up to date"
}

check_conflicting_daemons() {
    local conflict=false
    local managed_earport_pid=0

    # These projects may open the same proprietary AAP L2CAP channel. Never
    # delete or stop another project implicitly; tell the user exactly what
    # must be disabled and leave that decision to them.
    for unit in \
        airpods-handoff.service \
        librepods.service \
        librepods-daemon.service; do
        if systemctl --user is-active --quiet "$unit" 2>/dev/null ||
           systemctl --user is-enabled --quiet "$unit" 2>/dev/null; then
            print_error "Conflicting user service is active or enabled: $unit"
            echo "    Disable it first with:"
            echo "    systemctl --user disable --now $unit"
            conflict=true
        fi
    done

    if command -v pgrep &> /dev/null; then
        for process in airpods-handoff librepods; do
            if pgrep -x -- "$process" &> /dev/null; then
                print_error "A manually launched $process process is running"
                echo "    Stop it before installing EarPort Handoff."
                conflict=true
            fi
        done

        # An existing instance managed by our user unit is expected during an
        # upgrade and will be restarted below. Any other earport-daemon would
        # retain the D-Bus singleton name and make the service restart-loop.
        managed_earport_pid="$(
            systemctl --user show earport-daemon.service \
                --property=MainPID --value 2>/dev/null || true
        )"
        managed_earport_pid="${managed_earport_pid:-0}"
        while IFS= read -r process_pid; do
            if [ -n "$process_pid" ] &&
               [ "$process_pid" != "$managed_earport_pid" ]; then
                print_error "A manually launched earport-daemon process is running (PID $process_pid)"
                echo "    Stop it before installing this systemd-managed copy."
                conflict=true
            fi
        done < <(pgrep -x -- earport-daemon || true)
    fi

    if [ "$conflict" = true ]; then
        exit 1
    fi
}

install_daemon() {
    print_step "Installing daemon to $INSTALL_PREFIX..."

    meson install -C "$DAEMON_BUILD_DIR"

    print_success "Daemon installed"
}

enable_daemon_service() {
    print_step "Enabling systemd user service..."

    # Reload systemd user daemon
    systemctl --user daemon-reload

    # Enabling an already running unit does not reload a newly installed
    # executable, so restart explicitly for both fresh installs and upgrades.
    systemctl --user enable earport-daemon.service
    systemctl --user restart earport-daemon.service

    print_success "Daemon service enabled and started"
}

install_extension() {
    print_step "Installing GNOME Shell extension..."

    local staging_dir
    local backup_dir

    # Create extensions directory if it doesn't exist
    mkdir -p "$EXTENSIONS_ROOT"

    # Build the replacement beside the live extension. A copy or schema error
    # must leave the currently working version untouched.
    staging_dir="$(mktemp -d "$EXTENSIONS_ROOT/.earport-stage.XXXXXX")"
    backup_dir="$(mktemp -d "$EXTENSIONS_ROOT/.earport-backup.XXXXXX")"
    rmdir "$backup_dir"
    if ! cp -a "$SCRIPT_DIR/extension/." "$staging_dir/"; then
        remove_owned_directory "$staging_dir" "$EXTENSIONS_ROOT"
        print_error "Could not stage the GNOME extension"
        return 1
    fi

    # Compile GSettings schemas (required by getSettings())
    if command -v glib-compile-schemas &> /dev/null; then
        if ! glib-compile-schemas --strict "$staging_dir/schemas"; then
            remove_owned_directory "$staging_dir" "$EXTENSIONS_ROOT"
            print_error "Could not compile extension settings schemas"
            return 1
        fi
    else
        print_warning "glib-compile-schemas not found; using pre-compiled schemas"
    fi

    if [ ! -f "$staging_dir/schemas/gschemas.compiled" ]; then
        remove_owned_directory "$staging_dir" "$EXTENSIONS_ROOT"
        print_error "Staged extension has no compiled settings schema"
        return 1
    fi

    if [ -e "$EXTENSION_DIR" ] || [ -L "$EXTENSION_DIR" ]; then
        if ! mv -- "$EXTENSION_DIR" "$backup_dir"; then
            remove_owned_directory "$staging_dir" "$EXTENSIONS_ROOT"
            print_error "Could not preserve the currently installed GNOME extension"
            return 1
        fi
    fi
    if ! mv -- "$staging_dir" "$EXTENSION_DIR"; then
        if [ -e "$backup_dir" ] || [ -L "$backup_dir" ]; then
            if ! mv -- "$backup_dir" "$EXTENSION_DIR"; then
                print_error "Could not restore the previous extension from $backup_dir"
            fi
        fi
        if [ -d "$staging_dir" ]; then
            remove_owned_directory "$staging_dir" "$EXTENSIONS_ROOT"
        fi
        print_error "Could not activate the staged GNOME extension"
        return 1
    fi
    if [ -e "$backup_dir" ] || [ -L "$backup_dir" ]; then
        remove_owned_directory "$backup_dir" "$EXTENSIONS_ROOT"
    fi

    print_success "Extension installed to $EXTENSION_DIR"
}

enable_extension() {
    print_step "Enabling extension..."

    # Try to enable the extension
    if command -v gnome-extensions &> /dev/null; then
        if gnome-extensions enable "$EXTENSION_UUID"; then
            print_success "Extension enabled"
        else
            print_warning "GNOME Shell has not loaded the new extension yet"
            echo "    On Wayland, log out and back in, then run:"
            echo "    gnome-extensions enable $EXTENSION_UUID"
        fi
    else
        print_warning "Could not enable extension automatically"
        echo "    Please enable it manually via GNOME Extensions app"
    fi
}

print_completion() {
    echo ""
    echo -e "${GREEN}════════════════════════════════════════════${NC}"
    echo -e "${GREEN}    Installation completed successfully!    ${NC}"
    echo -e "${GREEN}════════════════════════════════════════════${NC}"
    echo ""
    echo "Next steps:"
    echo "  1. Restart GNOME Shell:"
    echo "     - Log out and log back in"
    echo ""
    echo "  2. Pair your AirPods via Bluetooth settings"
    echo ""
    echo "  3. The AirPods tile will appear in Quick Settings"
    echo ""
    echo "To check daemon status:"
    echo "  systemctl --user status earport-daemon.service"
    echo ""
    echo "To view daemon logs:"
    echo "  journalctl --user -u earport-daemon.service -f"
    echo ""
}

uninstall() {
    print_header
    print_step "Uninstalling AirPods Seamless Switching for Ubuntu..."

    # Stop and disable service
    print_step "Stopping daemon service..."
    systemctl --user disable --now earport-daemon.service 2>/dev/null || true
    print_success "Daemon service stopped"

    # Complete removal also stops copies launched by hand rather than by the
    # user unit. The exact process name belongs to this project.
    if command -v pgrep &> /dev/null &&
       pgrep -x -- earport-daemon &> /dev/null; then
        print_step "Stopping manually launched EarPort daemon..."
        pkill -x -- earport-daemon 2>/dev/null || true
        print_success "Manual daemon stopped"
    fi

    # Uninstall daemon
    print_step "Uninstalling daemon from $INSTALL_PREFIX..."
    # Use exact installed paths so uninstall still works from a fresh clone or
    # after the build directory has been removed.
    rm -f \
        "$INSTALL_PREFIX/bin/earport-daemon" \
        "$USER_DATA_HOME/systemd/user/earport-daemon.service" \
        "$USER_DATA_HOME/dbus-1/services/io.github.anoryth.EarPort.service"
    # Remove locations used by older releases when XDG_DATA_HOME differs from
    # ~/.local/share. These are exact project-owned files, never directories.
    if [ "$USER_DATA_HOME" != "$INSTALL_PREFIX/share" ]; then
        rm -f \
            "$INSTALL_PREFIX/share/systemd/user/earport-daemon.service" \
            "$INSTALL_PREFIX/share/dbus-1/services/io.github.anoryth.EarPort.service"
    fi
    systemctl --user daemon-reload 2>/dev/null ||
        print_warning "Could not reload the systemd user manager"
    print_success "Daemon uninstalled"

    # Remove extension
    print_step "Removing extension..."
    if command -v gnome-extensions &> /dev/null; then
        gnome-extensions disable "$EXTENSION_UUID" 2>/dev/null || true
    fi
    local settings_extension_dir="$EXTENSION_DIR"
    if [ ! -f "$settings_extension_dir/schemas/gschemas.compiled" ] &&
       [ -f "$LEGACY_EXTENSION_DIR/schemas/gschemas.compiled" ]; then
        settings_extension_dir="$LEGACY_EXTENSION_DIR"
    fi
    if command -v gsettings &> /dev/null &&
       [ -f "$settings_extension_dir/schemas/gschemas.compiled" ]; then
        gsettings --schemadir "$settings_extension_dir/schemas" \
            reset-recursively org.gnome.shell.extensions.earport \
            2>/dev/null || print_warning "Could not reset GNOME extension settings"
    fi
    if [ -d "$EXTENSION_DIR" ] || [ -L "$EXTENSION_DIR" ]; then
        remove_owned_directory "$EXTENSION_DIR" "$EXTENSIONS_ROOT"
    fi
    if [ "$LEGACY_EXTENSION_DIR" != "$EXTENSION_DIR" ] &&
       { [ -d "$LEGACY_EXTENSION_DIR" ] || [ -L "$LEGACY_EXTENSION_DIR" ]; }; then
        remove_owned_directory "$LEGACY_EXTENSION_DIR" "$LEGACY_EXTENSIONS_ROOT"
    fi
    # A terminated installer may have left a project-owned staging directory.
    # Remove only exact EarPort staging prefixes immediately below either
    # extensions root.
    for extensions_root in "$EXTENSIONS_ROOT" "$LEGACY_EXTENSIONS_ROOT"; do
        if [ -d "$extensions_root" ]; then
            while IFS= read -r -d '' stale_dir; do
                remove_owned_directory "$stale_dir" "$extensions_root"
            done < <(
                find "$extensions_root" -mindepth 1 -maxdepth 1 -type d \
                    \( -name '.earport-stage.*' -o -name '.earport-backup.*' \) \
                    -print0
            )
        fi
    done
    print_success "Extension removed"

    # Remove EarPort-owned per-user settings and saved device profiles. The
    # system-wide BlueZ identity and Bluetooth bonds are intentionally handled
    # separately because the installer did not create or own those files.
    print_step "Removing EarPort user configuration..."
    if [ -d "$USER_CONFIG_HOME/earport" ]; then
        remove_owned_directory "$USER_CONFIG_HOME/earport" "$USER_CONFIG_HOME"
    fi
    print_success "EarPort user configuration removed"

    if [ -d "$DAEMON_BUILD_DIR" ]; then
        remove_owned_directory "$DAEMON_BUILD_DIR" "$SCRIPT_DIR/daemon"
    fi

    echo ""
    echo -e "${GREEN}Uninstallation completed!${NC}"
    echo "Please log out and back in to unload the GNOME extension."
    echo ""
    echo "To restore the system Bluetooth identity, remove this line from"
    echo "/etc/bluetooth/main.conf and restart Bluetooth:"
    echo "  DeviceID = bluetooth:004C:0000:0000"
    echo "  sudoedit /etc/bluetooth/main.conf"
    echo "  sudo systemctl restart bluetooth"
    echo "Existing Bluetooth pairings are not deleted."
}

show_help() {
    echo "AirPods Seamless Switching for Ubuntu"
    echo "Supported platform: Ubuntu 26.04 or newer"
    echo ""
    echo "Usage: $0 [OPTION]"
    echo ""
    echo "Options:"
    echo "  --install     Install daemon and extension (default)"
    echo "  --update      Fast-forward Git, rebuild, and update everything"
    echo "  --uninstall   Remove daemon, extension, and user settings"
    echo "  --daemon      Install only the daemon"
    echo "  --extension   Install only the extension"
    echo "  --help        Show this help message"
    echo ""
}

# Main
main() {
    case "${1:-}" in
        --uninstall)
            uninstall
            ;;
        --update)
            print_header
            check_supported_platform
            update_checkout
            # Re-exec the freshly updated script so installer fixes from the
            # pulled revision take effect during this same update.
            exec "$SCRIPT_DIR/install.sh" --install
            ;;
        --daemon)
            print_header
            check_supported_platform
            check_dependencies
            check_conflicting_daemons
            build_daemon
            install_daemon
            enable_daemon_service
            echo ""
            print_success "Daemon installation completed!"
            ;;
        --extension)
            print_header
            check_supported_platform
            check_conflicting_daemons
            install_extension
            enable_extension
            echo ""
            print_success "Extension installation completed!"
            echo "Please restart GNOME Shell to load the extension."
            ;;
        --help|-h)
            show_help
            ;;
        --install|"")
            print_header
            check_supported_platform
            check_dependencies
            check_conflicting_daemons
            build_daemon
            install_daemon
            enable_daemon_service
            install_extension
            enable_extension
            print_completion
            ;;
        *)
            print_error "Unknown option: $1"
            show_help
            exit 1
            ;;
    esac
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi
