#!/usr/bin/env bash
# Packs an installed Hearth Engine SDK (cmake --install output) into a .deb.
#   scripts/hearth/make-deb.sh <install-dir> <output.deb>
# The version comes from scripts/hearth/VERSION (Dragongamer9 picks every version number).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
src="$(cd "$1" && pwd)"
out="$2"
version="$(tr -d ' \n' < "$here/VERSION")"
[ -n "$version" ] || { echo "scripts/hearth/VERSION is empty" >&2; exit 1; }

pkg="$(mktemp -d)/hearth-engine"
mkdir -p "$pkg/DEBIAN" "$pkg/opt" "$pkg/usr/bin" "$pkg/usr/share/applications" "$pkg/usr/share/icons/hicolor/scalable/apps"
# Hard links: no second copy of a multi-GB tree on the build machine.
cp -al "$src" "$pkg/opt/hearth-engine" 2>/dev/null || cp -a "$src" "$pkg/opt/hearth-engine"

# Command: hearth-engine (opens the project manager; the editor needs X11 on Hearth OS).
cat > "$pkg/usr/bin/hearth-engine" <<'LAUNCH'
#!/usr/bin/env bash
ENGINE=/opt/hearth-engine
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-xcb}"
# First run for this user (and after each upgrade): register the engine so projects can find it.
marker="${XDG_CONFIG_HOME:-$HOME/.config}/hearth-engine/registered-$(cat "$ENGINE/HEARTH_VERSION" 2>/dev/null)"
if [ ! -e "$marker" ] && [ -x "$ENGINE/scripts/o3de.sh" ]; then
    mkdir -p "$(dirname "$marker")"
    "$ENGINE/scripts/o3de.sh" register --this-engine >/dev/null 2>&1 && touch "$marker"
fi
for candidate in "$ENGINE"/bin/Linux/profile/Default/o3de "$ENGINE"/bin/Linux/release/Default/o3de; do
    if [ -x "$candidate" ]; then exec "$candidate" "$@"; fi
done
found="$(find "$ENGINE/bin" -type f -name o3de -perm -u+x 2>/dev/null | head -1)"
[ -n "$found" ] && exec "$found" "$@"
echo "Hearth Engine: project manager not found in $ENGINE/bin" >&2
exit 1
LAUNCH
chmod 755 "$pkg/usr/bin/hearth-engine"
echo "$version" > "$pkg/opt/hearth-engine/HEARTH_VERSION"

cp "$here/../../Code/Editor/o3de_logo.svg" "$pkg/usr/share/icons/hicolor/scalable/apps/hearth-engine.svg"
cat > "$pkg/usr/share/applications/hearth-engine.desktop" <<'DESK'
[Desktop Entry]
Type=Application
Name=Hearth Engine
GenericName=Game Engine
Comment=Make games with the Hearth Engine
Exec=hearth-engine
Icon=hearth-engine
Terminal=false
Categories=Development;Game;
StartupWMClass=o3de
DESK

size_kb="$(du -sk "$pkg" | cut -f1)"
cat > "$pkg/DEBIAN/control" <<CTRL
Package: hearth-engine
Version: $version
Architecture: amd64
Maintainer: Dragongamer9 <43799111+Dragongamer9@users.noreply.github.com>
Installed-Size: $size_kb
Depends: libc6, libstdc++6, libvulkan1, libxcb1, libxkbcommon-x11-0, libfontconfig1, zlib1g
Section: devel
Priority: optional
Homepage: https://github.com/Hearth-Organization/hearth-engine
Description: Hearth Engine game engine
 The Hearth Engine: a game engine built on O3DE (Apache-2.0), tuned for Hearth OS
 laptops and handhelds. Installs to /opt/hearth-engine and adds the hearth-engine
 command and an app menu entry.
CTRL
cat > "$pkg/DEBIAN/postinst" <<'POST'
#!/bin/sh
set -e
# The engine's Python setup (python/get_python.sh) runs "pip install -e scripts/o3de" as the user, which
# writes scripts/o3de/o3de.egg-info inside the engine folder. Give it a folder it can write to.
egg=/opt/hearth-engine/scripts/o3de/o3de.egg-info
mkdir -p "$egg" && chmod 1777 "$egg" || true
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q /usr/share/applications || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -t /usr/share/icons/hicolor || true
exit 0
POST
chmod 755 "$pkg/DEBIAN/postinst"
# Removing the package also removes what the Python setup wrote there.
cat > "$pkg/DEBIAN/postrm" <<'POSTRM'
#!/bin/sh
set -e
[ "$1" = remove ] || [ "$1" = purge ] && rm -rf /opt/hearth-engine/scripts/o3de/o3de.egg-info || true
exit 0
POSTRM
chmod 755 "$pkg/DEBIAN/postrm"

dpkg-deb --root-owner-group -Zzstd -z12 -b "$pkg" "$out"
ls -lh "$out"
