#!/usr/bin/env python3
"""Hearth Engine theme: recolours the engine's Qt style files from stock O3DE to the Hearth look.

Run from the repo root:  python3 scripts/hearth/apply-theme.py [--check]

Stock O3DE uses neutral greys and a blue accent. Hearth uses warm near-blacks, warm off-white text and an ember
accent (#e8743b). This script rewrites colours in .qss, style .ini and style .json files:
  - neutral greys (r == g == b) move onto the warm Hearth ramp, by brightness
  - blues (hue 180-260) become ember, keeping their brightness
  - every other colour (status reds, greens, yellows, data-type colours) is left alone
It also swaps the font Open Sans for Geist (bundled in AzQtComponents/Components/Fonts/Geist).
It's safe to run again: warm colours and embers are not neutral greys or blues, so they are not touched twice.
"""
import colorsys, os, re, sys

# Stock grey brightness -> Hearth colour (from the approved mockups).
GREY_RAMP = [
    (0x00, (0x00, 0x00, 0x00)),
    (0x11, (0x0d, 0x0c, 0x0b)),   # window background
    (0x1a, (0x11, 0x10, 0x0e)),
    (0x22, (0x14, 0x12, 0x10)),   # panels
    (0x2d, (0x16, 0x14, 0x12)),
    (0x33, (0x17, 0x15, 0x13)),   # cards
    (0x3a, (0x1c, 0x19, 0x17)),
    (0x44, (0x21, 0x1e, 0x1b)),   # inputs, list backgrounds
    (0x55, (0x2a, 0x26, 0x22)),
    (0x66, (0x35, 0x30, 0x2b)),   # lines, disabled
    (0x77, (0x4a, 0x44, 0x3d)),
    (0x88, (0x6e, 0x67, 0x5e)),
    (0x99, (0x8d, 0x85, 0x7b)),   # dim text
    (0xbb, (0xb8, 0xb0, 0xa6)),
    (0xcc, (0xd2, 0xcb, 0xc2)),
    (0xff, (0xec, 0xe6, 0xde)),   # text
]
# Exact stock accent colours -> Hearth colours.
ACCENTS = {
    "1e70eb": "e8743b",  # main accent
    "0095f2": "f08a55",
    "4abaff": "ffa877",
    "94d2ff": "ffcfb4",  # light accent (links, active icons)
    "4b8cef": "e8743b",  # focus border
    "2a4875": "4a2a1a",  # disabled accent / dark selection
    "ccdff5": "ffe4d4",
    "17a3cd": "e8743b",
}
EMBER_HUE = 20 / 360.0


def lerp(a, b, t):
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))


def map_grey(v):
    for (k0, c0), (k1, c1) in zip(GREY_RAMP, GREY_RAMP[1:]):
        if k0 <= v <= k1:
            return lerp(c0, c1, (v - k0) / (k1 - k0) if k1 != k0 else 0)
    return GREY_RAMP[-1][1]


def map_rgb(r, g, b):
    """Returns the new (r, g, b) or None to leave the colour alone."""
    hexs = "%02x%02x%02x" % (r, g, b)
    if GREYS_ONLY and not (r == g == b):
        return None
    if hexs in ACCENTS:
        a = ACCENTS[hexs]
        return int(a[0:2], 16), int(a[2:4], 16), int(a[4:6], 16)
    if r == g == b:
        return map_grey(r)
    h, l, s = colorsys.rgb_to_hls(r / 255, g / 255, b / 255)
    if 180 / 360 <= h <= 260 / 360 and s > 0.25:
        nr, ng, nb = colorsys.hls_to_rgb(EMBER_HUE, l, min(1.0, s * 0.95))
        return round(nr * 255), round(ng * 255), round(nb * 255)
    return None


HEX = re.compile(r"#([0-9a-fA-F]{8}|[0-9a-fA-F]{6}|[0-9a-fA-F]{3})\b")
RGB = re.compile(r"\brgba?\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*(,\s*[\d.]+%?\s*)?\)")


ALPHA_FIRST = False   # set per file: Qt .ini files write #AARRGGBB, .qss and .json write #RRGGBBAA
GREYS_ONLY = False    # set per file: graph styles keep their data-type colours, only the greys change


def sub_hex(m):
    s = m.group(1)
    pre = post = ""
    if len(s) == 3:
        r, g, b = (int(c * 2, 16) for c in s)
    elif len(s) == 8:
        if ALPHA_FIRST:
            pre, s6 = s[0:2], s[2:8]
        else:
            s6, post = s[0:6], s[6:8]
        r, g, b = int(s6[0:2], 16), int(s6[2:4], 16), int(s6[4:6], 16)
    else:
        r, g, b = int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16)
    new = map_rgb(r, g, b)
    if new is None or tuple(new) == (r, g, b):
        return m.group(0)
    body = "%02X%02X%02X" % new
    if not any(ch.isupper() for ch in s) and any(ch.isalpha() for ch in s):
        body = body.lower()
    return "#" + pre + body + post


def sub_rgb(m):
    old = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
    new = map_rgb(*old)
    if new is None or tuple(new) == old:
        return m.group(0)
    fn = "rgba" if m.group(4) else "rgb"
    return "%s(%d, %d, %d%s)" % (fn, new[0], new[1], new[2], m.group(4) or "")


# Which files are the engine's look (tools for DCC apps and test data are left alone).
ROOTS = ["Code/Framework/AzQtComponents", "Code/Framework/AzToolsFramework", "Code/Editor", "Code/Tools",
         "Assets/Editor/Styles", "Gems"]
SKIP = ["/Tests/", "/DccScriptingInterface/", "/Gallery/", "/test", "/External/", "/Hearth.qss"]
# Axis colours (X red, Y green, Z blue) must stay as they are.
GREYS_ONLY_FILES = ["VectorInput.qss"]
EXTS = (".qss", ".ini", ".json")


def style_file(path):
    p = path.replace(os.sep, "/")
    if any(s in p for s in SKIP) or not p.endswith(EXTS):
        return False
    if p.endswith(".ini"):
        return "Config" in os.path.basename(p) or "/Style" in p
    if p.endswith(".json"):
        return "/StyleSheet/" in p or "StyleSheetVariables" in p or "/Styles/" in p
    return True


def main():
    global ALPHA_FIRST, GREYS_ONLY
    check = "--check" in sys.argv
    changed = []
    for root in ROOTS:
        for dp, _, fs in os.walk(root):
            for f in fs:
                path = os.path.join(dp, f)
                if not style_file(path):
                    continue
                ALPHA_FIRST = path.endswith(".ini")
                GREYS_ONLY = "/StyleSheet/" in path.replace(os.sep, "/") or os.path.basename(path) in GREYS_ONLY_FILES
                text = open(path, encoding="utf-8", errors="surrogateescape").read()
                new = RGB.sub(sub_rgb, HEX.sub(sub_hex, text)).replace("Open Sans", "Geist").replace("Amazon Ember", "Geist")
                if new != text:
                    changed.append(path)
                    if not check:
                        open(path, "w", encoding="utf-8", errors="surrogateescape").write(new)
    for p in changed:
        print(("would change " if check else "changed ") + p)
    print(len(changed), "files")
    return 1 if (check and changed) else 0


if __name__ == "__main__":
    sys.exit(main())
