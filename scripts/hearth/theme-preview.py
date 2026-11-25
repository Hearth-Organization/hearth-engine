#!/usr/bin/env python3
"""Hearth Engine theme preview: renders the engine's stylesheet on a sample editor window, without building the engine.

  pip install PySide6
  QT_QPA_PLATFORM=offscreen python3 scripts/hearth/theme-preview.py out.png

It loads AzQtComponents' BaseStyleSheet.qss the way the engine does (following @import, Geist fonts registered),
so a broken rule or a stock-looking colour shows up here before a 30-minute engine build.
Buttons and line edit frames are painted in C++ in the engine (PushButton.cpp, LineEdit.cpp), so here they only
approximate the real thing.
"""
import os, re, sys
from PySide6 import QtCore, QtGui, QtWidgets

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
WIDGETS = os.path.join(ROOT, "Code/Framework/AzQtComponents/AzQtComponents/Components/Widgets")
FONTS = os.path.join(ROOT, "Code/Framework/AzQtComponents/AzQtComponents/Components/Fonts/Geist")


def load_qss(name, seen=None):
    seen = seen or set()
    if name in seen:
        return ""
    seen.add(name)
    text = open(os.path.join(WIDGETS, name), encoding="utf-8").read()
    return re.sub(r'@import\s+"([^"]+)";', lambda m: load_qss(m.group(1), seen), text)


def build():
    win = QtWidgets.QMainWindow()
    win.setWindowTitle("Hearth Engine")
    mb = win.menuBar()
    for t in ["File", "Edit", "Level", "Create", "Tools", "Game", "View", "Help"]:
        m = mb.addMenu(t)
        m.addAction("Level Generator")
    tb = win.addToolBar("main")
    for t in ["Select", "Move", "Rotate", "Scale"]:
        a = tb.addAction(t)
        a.setCheckable(True)
        a.setChecked(t == "Move")

    central = QtWidgets.QWidget()
    lay = QtWidgets.QVBoxLayout(central)
    tabs = QtWidgets.QTabWidget()
    view = QtWidgets.QLabel("Viewport")
    view.setAlignment(QtCore.Qt.AlignCenter)
    view.setStyleSheet("background:#2a2616;border-radius:12px;color:#8d857b;min-height:300px")
    tabs.addTab(view, "Level0")
    tabs.addTab(QtWidgets.QWidget(), "Lighting and look")
    lay.addWidget(tabs)
    console = QtWidgets.QPlainTextEdit("21:04:12  Level0 loaded in 1.8 s\n21:04:13  Asset processing: 0 jobs left\n"
                                       "21:04:15  [warning] cloudnoise_01 failed to process")
    console.setProperty("HearthMono", True)
    console.setMaximumHeight(110)
    lay.addWidget(console)
    win.setCentralWidget(central)

    left = QtWidgets.QDockWidget("Level")
    tree = QtWidgets.QTreeWidget()
    tree.setHeaderLabels(["Name"])
    root = QtWidgets.QTreeWidgetItem(["Level0"])
    for n in ["Spawn room", "Endless level", "Ceiling light 04", "Player start"]:
        root.addChild(QtWidgets.QTreeWidgetItem([n]))
    tree.addTopLevelItem(root)
    root.setExpanded(True)
    tree.setCurrentItem(root.child(2))
    left.setWidget(tree)
    win.addDockWidget(QtCore.Qt.LeftDockWidgetArea, left)

    right = QtWidgets.QDockWidget("Inspector")
    w = QtWidgets.QWidget()
    f = QtWidgets.QFormLayout(w)
    f.addRow("Name", QtWidgets.QLineEdit("Ceiling light 04"))
    sp = QtWidgets.QDoubleSpinBox(); sp.setValue(2.5)
    f.addRow("Intensity", sp)
    cb = QtWidgets.QComboBox(); cb.addItems(["Flickering", "Working", "Dead"])
    f.addRow("State", cb)
    chk = QtWidgets.QCheckBox("Hum sound"); chk.setChecked(True)
    f.addRow("", chk)
    sl = QtWidgets.QSlider(QtCore.Qt.Horizontal); sl.setValue(40)
    f.addRow("Volume", sl)
    pb = QtWidgets.QProgressBar(); pb.setValue(64)
    f.addRow("Processing", pb)
    gb = QtWidgets.QGroupBox("Light")
    gl = QtWidgets.QHBoxLayout(gb)
    b1 = QtWidgets.QPushButton("Add component"); b1.setProperty("class", "Primary"); b1.setDefault(True)
    gl.addWidget(b1)
    gl.addWidget(QtWidgets.QPushButton("Reset"))
    f.addRow(gb)
    right.setWidget(w)
    win.addDockWidget(QtCore.Qt.RightDockWidgetArea, right)
    win.statusBar().showMessage("Ready · 48 fps · Claude can read these logs")
    win.resize(1440, 900)
    return win


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "theme-preview.png"
    app = QtWidgets.QApplication(sys.argv)
    for f in os.listdir(FONTS):
        if f.endswith(".ttf"):
            QtGui.QFontDatabase.addApplicationFont(os.path.join(FONTS, f))
    font = QtGui.QFont("Geist"); font.setPixelSize(12)
    app.setFont(font)
    app.setStyle("Fusion")
    pal = app.palette()   # same colours Style::polish sets in the engine
    for role, c in [("Window", "#0D0C0B"), ("WindowText", "#ECE6DE"), ("Base", "#1C1917"), ("AlternateBase", "#171513"),
                    ("Text", "#ECE6DE"), ("Button", "#1C1917"), ("ButtonText", "#ECE6DE"), ("Highlight", "#E8743B"),
                    ("HighlightedText", "#1A0F08"), ("ToolTipBase", "#1D1A17"), ("ToolTipText", "#ECE6DE")]:
        pal.setColor(getattr(QtGui.QPalette.ColorRole, role), QtGui.QColor(c))
    app.setPalette(pal)
    app.setStyleSheet(load_qss("BaseStyleSheet.qss"))
    win = build()
    win.show()
    app.processEvents()
    win.grab().save(out)
    print("saved", out)


if __name__ == "__main__":
    main()
