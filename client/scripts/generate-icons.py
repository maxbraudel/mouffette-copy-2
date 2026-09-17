#!/usr/bin/env python3
"""Regenerate the native icons from resources/icons/logo/mouffette-logo.svg.

Run on macOS with Python 3, Pillow, librsvg's rsvg-convert and Apple's iconutil.
The generated files are checked in; ordinary builds need none of these tools.
"""

import copy
import io
from pathlib import Path
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET

from PIL import Image


LOGO_DIR = Path(__file__).resolve().parents[1] / "resources/icons/logo"
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256, 512, 1024)
SVG_NS = "http://www.w3.org/2000/svg"


def render(svg: bytes, size: int) -> Image.Image:
    result = subprocess.run(
        ["rsvg-convert", "--width", str(size), "--height", str(size)],
        input=svg, stdout=subprocess.PIPE, check=True,
    )
    return Image.open(io.BytesIO(result.stdout)).convert("RGBA")


def tray_template(svg: bytes) -> bytes:
    """Keep the badge and every path; punch out the logo's light ink.

    A macOS template uses alpha, not its RGB values. Simply marking the full
    colour badge as a template would render an opaque, featureless circle.
    """
    source = ET.fromstring(svg)
    view_box = source.attrib["viewBox"]
    x, y, width, height = view_box.split()
    bounds = dict(x=x, y=y, width=width, height=height)
    root = ET.Element(f"{{{SVG_NS}}}svg", viewBox=view_box)
    defs = ET.SubElement(root, f"{{{SVG_NS}}}defs")
    mask = ET.SubElement(defs, f"{{{SVG_NS}}}mask", id="ink",
                         maskUnits="userSpaceOnUse", **bounds)
    for child in source:
        if child.tag.rsplit("}", 1)[-1] in ("title", "desc"):
            continue
        shape = copy.deepcopy(child)
        for element in shape.iter():
            fill = element.get("fill")
            if fill is not None:
                if fill not in ("#101010", "#f7f5ef"):
                    raise ValueError(f"Update the tray ink mapping for new colour {fill}")
                element.set("fill", "white" if fill == "#101010" else "black")
        mask.append(shape)
    ET.SubElement(root, f"{{{SVG_NS}}}rect", fill="black",
                  mask="url(#ink)", **bounds)
    return ET.tostring(root)


def main() -> None:
    for tool in ("rsvg-convert", "iconutil"):
        if not shutil.which(tool):
            raise SystemExit(f"Required tool not found: {tool} (see the logo README)")

    source = (LOGO_DIR / "mouffette-logo.svg").read_bytes()
    template = tray_template(source)
    images = {size: render(source, size) for size in SIZES}
    for size, image in images.items():
        image.save(LOGO_DIR / f"mouffette-{size}.png")

    ico_sizes = [size for size in SIZES if size <= 256]
    images[256].save(
        LOGO_DIR / "mouffette.ico", format="ICO",
        sizes=[(size, size) for size in ico_sizes],
        append_images=[images[size] for size in ico_sizes],
    )

    with tempfile.TemporaryDirectory(prefix="mouffette-icons-") as temporary:
        iconset = Path(temporary) / "mouffette.iconset"
        iconset.mkdir()
        for size in (16, 32, 128, 256, 512):
            images[size].save(iconset / f"icon_{size}x{size}.png")
            images[size * 2].save(iconset / f"icon_{size}x{size}@2x.png")
        subprocess.run(
            ["iconutil", "--convert", "icns", "--output",
             str(LOGO_DIR / "mouffette.icns"), str(iconset)], check=True,
        )

    render(template, 18).save(LOGO_DIR / "mouffette-tray-macos.png")
    render(template, 36).save(LOGO_DIR / "mouffette-tray-macos@2x.png")
    print(f"Generated PNG, ICO, ICNS and macOS tray templates in {LOGO_DIR}")


if __name__ == "__main__":
    main()
