#!/bin/sh
# Put Speechify in this user's applications menu.
#
# The tarball is relocatable - it can be extracted anywhere - so the .desktop
# file cannot be shipped ready-made: Exec= has to be an absolute path, and
# nobody knows it until extraction. This resolves the path at run time and
# writes the file.
#
# Per-user (~/.local/share) rather than system-wide (/usr/share) ON PURPOSE:
# no root, nothing to undo outside $HOME, and it matches where the tarball
# itself lives. Run it with --uninstall to remove what it wrote.
#
# ⚠ POSIX sh, no bashisms and no heredocs - printf writes the file. This has
# to run on Alpine's ash and on a Pi as readily as on bash.
set -eu

# The package root is this script's parent directory. `cd`-then-`pwd` rather
# than a dirname string so a relative invocation ("sh ./install-desktop.sh")
# still yields an absolute Exec=.
HERE=$(cd "$(dirname "$0")" && pwd)
GUI="${HERE}/bin/spfy_gui"

APPS="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
ICONS="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/256x256/apps"
ENTRY="${APPS}/speechify.desktop"
ICON="${ICONS}/speechify.png"

if [ "${1:-}" = "--uninstall" ]; then
    rm -f "$ENTRY" "$ICON"
    echo "removed $ENTRY"
    echo "removed $ICON"
elif [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
    echo "usage: $0 [--uninstall]"
    echo
    echo "Adds Speechify to this user's applications menu, pointing at"
    echo "${GUI}."
    exit 0
else
    if [ ! -x "$GUI" ]; then
        echo "error: no spfy_gui at ${GUI}" >&2
        echo "       run this from inside the extracted package." >&2
        exit 1
    fi

    mkdir -p "$APPS" "$ICONS"

    # ⚠ The icon is referenced by NAME ("Icon=speechify"), not by path, so it
    # has to land in the hicolor theme for the menu to resolve it. A path
    # works on some desktops and silently shows nothing on others.
    if [ -f "${HERE}/share/speechify.png" ]; then
        cp "${HERE}/share/speechify.png" "$ICON"
    fi

    # ⚠ Categories: EXACTLY ONE main category. AudioVideo and Utility are both
    # "main" in the freedesktop spec and listing two makes the app appear
    # TWICE in some menus -- desktop-file-validate warns about it. Audio and
    # Accessibility are additional categories, which is what they should be.
    # validate still hints that Accessibility is usually paired with Settings
    # or Utility; both are main categories, so taking that hint would bring
    # the double-listing back. Accessibility stays, because accessibility
    # menus filter on it and that is this app's most likely audience.
    #
    # ⚠ StartupWMClass must match the class the toolkit reports, which for
    # Tauri is the BINARY NAME, not the window title. Wrong here and a
    # launched window opens as an orphan instead of grouping with the
    # taskbar entry.
    printf '%s\n' \
        '[Desktop Entry]' \
        'Type=Application' \
        'Version=1.0' \
        'Name=Speechify' \
        'GenericName=Text to Speech' \
        'Comment=Pick a voice, type text, hear it, and save the WAV' \
        "Exec=${GUI} %f" \
        "TryExec=${GUI}" \
        'Icon=speechify' \
        'Terminal=false' \
        'Categories=AudioVideo;Audio;Accessibility;' \
        'Keywords=tts;speech;voice;synthesis;narrator;sapi;' \
        'MimeType=text/plain;' \
        'StartupNotify=true' \
        'StartupWMClass=spfy_gui' \
        > "$ENTRY"
    chmod 644 "$ENTRY"

    echo "wrote $ENTRY"
    [ -f "$ICON" ] && echo "wrote $ICON"
fi

# Both are best-effort: most desktops pick the change up on their own, and a
# missing tool is not a failure worth aborting an otherwise complete install.
command -v update-desktop-database >/dev/null 2>&1 &&
    update-desktop-database "$APPS" 2>/dev/null || true
command -v gtk-update-icon-cache >/dev/null 2>&1 &&
    gtk-update-icon-cache -q -t -f "${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor" 2>/dev/null || true

exit 0
