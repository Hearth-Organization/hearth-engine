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
# Hearth Engine launcher. Everything the engine prints goes to a log file as well as the terminal,
# so problems can be read afterwards (by you or by Claude): ~/O3DE/HearthEngine/logs/
ENGINE=/opt/hearth-engine
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-xcb}"
LOGS="${HEARTH_ENGINE_LOGS:-$HOME/O3DE/HearthEngine/logs}"
mkdir -p "$LOGS"
stamp="$(date +%Y-%m-%d_%H-%M-%S)"
tool="${HEARTH_ENGINE_TOOL:-o3de}"
log="$LOGS/$stamp-$tool.log"
ln -sfn "$(basename "$log")" "$LOGS/latest.log"
# Keep the newest 30 logs.
ls -1t "$LOGS"/*.log 2>/dev/null | grep -v '/latest.log$' | tail -n +31 | xargs -r rm -f
{
    echo "== Hearth Engine $(cat "$ENGINE/HEARTH_VERSION" 2>/dev/null) · $tool · $(date) =="
    echo "== args: $* =="
    echo "== $(uname -sr) · $(grep -m1 PRETTY_NAME /etc/os-release 2>/dev/null | cut -d= -f2) =="
} >> "$log"
run() { "$@" 2>&1 | tee -a "$log"; return "${PIPESTATUS[0]}"; }

# First run for this user (and after each upgrade): register the engine so projects can find it.
marker="${XDG_CONFIG_HOME:-$HOME/.config}/hearth-engine/registered-$(cat "$ENGINE/HEARTH_VERSION" 2>/dev/null)"
if [ ! -e "$marker" ] && [ -x "$ENGINE/scripts/o3de.sh" ]; then
    mkdir -p "$(dirname "$marker")"
    echo "== registering the engine ==" >> "$log"
    run "$ENGINE/scripts/o3de.sh" register --this-engine >/dev/null && touch "$marker"
fi

exe=""
for candidate in "$ENGINE"/bin/Linux/profile/Default/"$tool" "$ENGINE"/bin/Linux/release/Default/"$tool"; do
    [ -x "$candidate" ] && exe="$candidate" && break
done
[ -z "$exe" ] && exe="$(find "$ENGINE/bin" -type f -name "$tool" -perm -u+x 2>/dev/null | head -1)"
if [ -z "$exe" ]; then
    echo "Hearth Engine: $tool not found in $ENGINE/bin" | tee -a "$log" >&2
    exit 1
fi
run "$exe" "$@"
code=$?
echo "== exited with code $code at $(date) ==" >> "$log"
# Also collect the engine's own log files from this run (project manager, editor, asset processor).
for own in "$HOME"/.o3de/Logs/*.log "$ENGINE"/bin/Linux/*/Default/user/log/*.log; do
    [ -f "$own" ] || continue
    if [ "$(find "$own" -newermt "-10 minutes" 2>/dev/null)" ]; then
        { echo; echo "== engine log: $own =="; tail -n 400 "$own"; } >> "$log"
    fi
done
exit $code
LAUNCH
chmod 755 "$pkg/usr/bin/hearth-engine"
# Same launcher (and logging) for the editor and the asset processor.
printf '#!/bin/sh\nHEARTH_ENGINE_TOOL=Editor exec hearth-engine "$@"\n' > "$pkg/usr/bin/hearth-engine-editor"
printf '#!/bin/sh\nHEARTH_ENGINE_TOOL=AssetProcessor exec hearth-engine "$@"\n' > "$pkg/usr/bin/hearth-engine-assets"
chmod 755 "$pkg/usr/bin/hearth-engine-editor" "$pkg/usr/bin/hearth-engine-assets"
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
# The project manager and tools keep their cache, settings and logs in <bin>/user when no project is open.
for bin in /opt/hearth-engine/bin/Linux/*/Default; do
    [ -d "$bin" ] && mkdir -p "$bin/user" && chmod 1777 "$bin/user" || true
done
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q /usr/share/applications || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -t /usr/share/icons/hicolor || true
exit 0
POST
chmod 755 "$pkg/DEBIAN/postinst"
# Removing the package also removes what the Python setup wrote there.
cat > "$pkg/DEBIAN/postrm" <<'POSTRM'
#!/bin/sh
set -e
if [ "$1" = remove ] || [ "$1" = purge ]; then
    rm -rf /opt/hearth-engine/scripts/o3de/o3de.egg-info /opt/hearth-engine/bin/Linux/*/Default/user || true
fi
exit 0
POSTRM
chmod 755 "$pkg/DEBIAN/postrm"

dpkg-deb --root-owner-group -Zzstd -z12 -b "$pkg" "$out"
ls -lh "$out"
