#!/usr/bin/env python3
"""Hearth Engine icons: writes the Hearth flame in place of the O3DE logo in the editor and tools.

  pip install PySide6 Pillow
  QT_QPA_PLATFORM=offscreen python3 scripts/hearth/make-icons.py

The flame and tile are the ones in the approved mockups (ember #E8743B tile, ink #1A0F08 flame).
File names stay the same so the code that loads them doesn't change.
"""
import io, os
from PySide6 import QtCore, QtGui, QtSvg, QtWidgets
from PIL import Image

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FLAME = "M9 1 C12 5 14 7 14 11 A5 5 0 0 1 4 11 C4 8 6 7 7 4 C8 6 9 6 9 1 Z"

# Ember tile with the ink flame (app icons).
TILE = f'''<?xml version="1.0" encoding="utf-8"?>
<!-- Hearth Engine icon. Hearth Engine is built on O3DE (Apache-2.0 / MIT). -->
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {{w}} {{h}}">
  <g transform="translate({{ox}} {{oy}}) scale({{s}})">
    <rect x="0" y="0" width="30" height="30" rx="9" fill="#E8743B"/>
    <g transform="translate(6 6) scale(1)"><path d="{FLAME}" fill="#1A0F08" transform="scale(1)"/></g>
  </g>
</svg>
'''
# Just the flame in ember (small icons on dark bars).
BARE = f'''<?xml version="1.0" encoding="utf-8"?>
<!-- Hearth Engine flame. Hearth Engine is built on O3DE (Apache-2.0 / MIT). -->
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {{w}} {{h}}">
  <g transform="translate({{ox}} {{oy}}) scale({{s}})"><path d="{FLAME}" fill="#E8743B"/></g>
</svg>
'''


def fit(template, w, h, size, inner):
    s = min(w, h) / size
    ox = (w - inner * s) / 2
    oy = (h - inner * s) / 2
    return template.format(w=w, h=h, ox=round(ox, 3), oy=round(oy, 3), s=round(s, 4))


def tile_svg(w, h):
    # tile is 30 units, the flame inside is 18 units (6..24) wide
    return fit(TILE, w, h, 30, 30)


def bare_svg(w, h):
    # flame path spans x 4..14, y 1..16 -> draw the 18-unit box
    return fit(BARE, w, h, 18, 18)


def render(svg, px):
    r = QtSvg.QSvgRenderer(QtCore.QByteArray(svg.encode()))
    img = QtGui.QImage(px, px, QtGui.QImage.Format_ARGB32)
    img.fill(QtCore.Qt.transparent)
    p = QtGui.QPainter(img)
    p.setRenderHint(QtGui.QPainter.Antialiasing)
    r.render(p)
    p.end()
    buf = QtCore.QBuffer()
    buf.open(QtCore.QIODevice.WriteOnly)
    img.save(buf, "PNG")
    return Image.open(io.BytesIO(bytes(buf.data())))


def write(rel, data, mode="w"):
    path = os.path.join(ROOT, rel)
    with open(path, mode) as f:
        f.write(data)
    print("wrote", rel)


def main():
    QtWidgets.QApplication([])
    write("Code/Editor/res/o3de_application_reverse.svg", bare_svg(24, 21))
    write("Code/Tools/AssetProcessor/native/ui/style/o3de_assetprocessor_taskbar.svg", bare_svg(22, 22))
    write("Code/Tools/ProjectManager/Resources/o3de.svg", tile_svg(38, 32))
    write("Code/Tools/ProjectManager/Resources/ProjectManager-Icon.svg", tile_svg(64, 64))
    big = render(tile_svg(256, 256), 256)
    for rel in ["Code/Editor/res/o3de_editor.ico", "Code/Tools/AssetProcessor/native/ui/style/o3de_assetprocessor.ico",
                "Code/Tools/ProjectManager/Resources/ProjectManager-Icon.ico"]:
        out = io.BytesIO()
        big.save(out, format="ICO", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
        write(rel, out.getvalue(), "wb")
    out = io.BytesIO()
    render(tile_svg(64, 64), 64).save(out, format="PNG")
    write("Code/Tools/AssetProcessor/native/ui/style/o3de_assetprocessor.png", out.getvalue(), "wb")


if __name__ == "__main__":
    main()
