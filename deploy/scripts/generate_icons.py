#!/usr/bin/env python3
"""Generate native application icons and installer artwork from one PNG."""

from __future__ import annotations

import argparse
import hashlib
import io
import os
import json
import math
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SOURCE = ROOT / "deploy" / "assets" / "app-icon.png"
DEFAULT_CONFIG = ROOT / "deploy" / "assets" / "installer-branding.json"
# What the committed outputs were drawn from: the source and the
# configuration, by their SHA-256. While it holds, a build writes nothing: the
# same source drawn by another computer's Pillow would differ byte for byte,
# and an ordinary build must change no file in git on any system. A change to
# how this script draws is drawn with --force and committed with its outputs.
DEFAULT_STAMP = ROOT / "deploy" / "assets" / "branding.sha256"

# Pillow draws. It is imported only when something is to be drawn, so a
# computer without it still learns that the committed set is up to date.
Image = ImageChops = ImageCms = ImageDraw = ImageFilter = ImageFont = None
SRGB_PROFILE = b""


def load_pillow() -> None:
    global Image, ImageChops, ImageCms, ImageDraw, ImageFilter, ImageFont, SRGB_PROFILE
    try:
        from PIL import Image, ImageChops, ImageCms, ImageDraw, ImageFilter, ImageFont
    except ImportError:
        print(
            "Pillow is required to draw the branding. Run: "
            "python3 -m pip install -r deploy/scripts/requirements-icons.txt",
            file=sys.stderr,
        )
        raise SystemExit(2)
    SRGB_PROFILE = ImageCms.ImageCmsProfile(ImageCms.createProfile("sRGB")).tobytes()


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate native icons and installer artwork from one square PNG"
    )
    parser.add_argument(
        "--source",
        type=Path,
        default=DEFAULT_SOURCE,
        help="Square source PNG or SVG (default: deploy/assets/app-icon.png)",
    )
    parser.add_argument(
        "--config",
        type=Path,
        default=DEFAULT_CONFIG,
        help="Installer text and color configuration (default: assets/installer-branding.json)",
    )
    parser.add_argument(
        "--skip-dmg",
        action="store_true",
        help="Keep the existing DMG background (its multi-resolution TIFF needs macOS)",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Draw everything again even when the source and configuration are unchanged",
    )
    return parser.parse_args()


def fingerprint(source_path: Path, config_path: Path) -> str:
    """What the outputs are drawn from: the source and the configuration."""
    digest = hashlib.sha256()
    for label, path in (("source", source_path), ("config", config_path)):
        digest.update(label.encode("utf-8"))
        digest.update(hashlib.sha256(path.read_bytes()).hexdigest().encode("ascii"))
    return digest.hexdigest()


def resized(source: Image.Image, size: int) -> Image.Image:
    return source.resize((size, size), Image.Resampling.LANCZOS)


def foreground_icon(source: Image.Image) -> Image.Image:
    """Export the selected head through its production silhouette.

    The generated raster's alpha contains disconnected matte fragments. This
    vector contour is the native icon's mask, in the master's 1254-unit grid;
    it excludes those fragments without touching the illustration's interior.
    Update the contour with the artwork when the character's shape changes.
    """
    start = (226, 95)
    curves = (
        ((175, 80), (130, 150), (97, 233)),
        ((29, 380), (16, 493), (53, 603)),
        ((72, 662), (100, 706), (143, 737)),
        ((115, 773), (68, 794), (15, 812)),
        ((14, 840), (58, 891), (97, 901)),
        ((84, 903), (68, 902), (58, 903)),
        ((64, 948), (124, 998), (185, 1031)),
        ((292, 1096), (467, 1146), (623, 1146)),
        ((808, 1146), (970, 1099), (1080, 1031)),
        ((1140, 995), (1182, 949), (1196, 905)),
        ((1184, 904), (1169, 902), (1159, 900)),
        ((1190, 887), (1229, 853), (1240, 813)),
        ((1182, 799), (1139, 775), (1114, 741)),
        ((1169, 698), (1198, 632), (1210, 556)),
        ((1240, 405), (1181, 202), (1119, 128)),
        ((1096, 99), (1076, 91), (1049, 95)),
        ((998, 104), (941, 162), (893, 210)),
        ((824, 279), (768, 343), (713, 414)),
        ((711, 357), (686, 311), (645, 297)),
        ((619, 290), (605, 307), (598, 345)),
        ((588, 332), (572, 332), (565, 344)),
        ((558, 360), (565, 394), (558, 433)),
        ((523, 385), (460, 295), (407, 241)),
        ((344, 176), (279, 108), start),
    )
    size, supersample = 1024, 4
    scale = size * supersample / 1254
    points = [start]
    current = start
    for first, second, end in curves:
        for step in range(1, 97):
            t = step / 96
            u = 1 - t
            points.append(tuple(
                u ** 3 * current[axis] + 3 * u ** 2 * t * first[axis]
                + 3 * u * t ** 2 * second[axis] + t ** 3 * end[axis]
                for axis in (0, 1)
            ))
        current = end
    mask = Image.new("L", (size * supersample, size * supersample), 0)
    ImageDraw.Draw(mask).polygon([(x * scale, y * scale) for x, y in points], fill=255)
    mask = mask.resize((size, size), Image.Resampling.LANCZOS)
    icon = resized(source, size)
    icon.putalpha(ImageChops.multiply(icon.getchannel("A"), mask))
    icon.info["icc_profile"] = SRGB_PROFILE
    return icon


def macos_compat_icon(source: Image.Image) -> Image.Image:
    """Mask the opaque Mac composition for the current ICNS bundle."""
    canvas_size = 1024
    tile_size = 896
    supersample = 4
    mask_size = tile_size * supersample
    mask = Image.new("L", (mask_size, mask_size), 0)
    radius = (mask_size - 1) / 2
    points = []
    for step in range(1440):
        angle = step * 2 * math.pi / 1440
        x, y = math.cos(angle), math.sin(angle)
        points.append((
            radius + radius * math.copysign(abs(x) ** 0.5, x),
            radius + radius * math.copysign(abs(y) ** 0.5, y),
        ))
    ImageDraw.Draw(mask).polygon(points, fill=255)
    mask = mask.resize((tile_size, tile_size), Image.Resampling.LANCZOS)

    tile = resized(source, tile_size)
    tile.putalpha(ImageChops.multiply(tile.getchannel("A"), mask))
    canvas = Image.new("RGBA", (canvas_size, canvas_size), (0, 0, 0, 0))
    canvas.info["icc_profile"] = SRGB_PROFILE
    inset = (canvas_size - tile_size) // 2
    canvas.alpha_composite(tile, (inset, inset))
    return canvas


def save_icns(source: Image.Image, destination: Path) -> None:
    """Include both ordinary and Retina PNG representations.

    Pillow's ICNS writer omits the 16-pixel 1x image even when append_images
    provides it. These standard PNG chunks also cover non-Retina displays.
    """
    representations = (
        (b"icp4", 16), (b"icp5", 32), (b"icp6", 64),
        (b"ic07", 128), (b"ic08", 256), (b"ic09", 512), (b"ic10", 1024),
        (b"ic11", 32), (b"ic12", 64), (b"ic13", 256), (b"ic14", 512),
    )
    pngs = {}
    for size in {size for _, size in representations}:
        stream = io.BytesIO()
        resized(source, size).save(stream, format="PNG", optimize=True)
        pngs[size] = stream.getvalue()
    chunks = []
    table = []
    for kind, size in representations:
        payload = pngs[size]
        header = kind + struct.pack(">I", len(payload) + 8)
        table.append(header)
        chunks.append(header + payload)
    toc = b"".join(table)
    payload = b"TOC " + struct.pack(">I", len(toc) + 8) + toc + b"".join(chunks)
    destination.write_bytes(b"icns" + struct.pack(">I", len(payload) + 8) + payload)


def font(size: int, *, bold: bool = False) -> ImageFont.ImageFont:
    names = (
        "Arial Bold.ttf" if bold else "Arial.ttf",
        "DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf",
    )
    for name in names:
        try:
            return ImageFont.truetype(name, size=size)
        except OSError:
            continue
    return ImageFont.load_default(size=size)


def fitted_font(
    text: str, max_width: int, start_size: int, *, min_size: int, bold: bool = False
) -> ImageFont.ImageFont:
    measuring_draw = ImageDraw.Draw(Image.new("RGB", (1, 1)))
    for size in range(start_size, min_size - 1, -1):
        candidate = font(size, bold=bold)
        bounds = measuring_draw.textbbox((0, 0), text, font=candidate)
        if bounds[2] - bounds[0] <= max_width:
            return candidate
    return font(min_size, bold=bold)


def color(value: str) -> tuple[int, ...]:
    normalized = value.removeprefix("#")
    if len(normalized) not in (6, 8):
        raise ValueError(f"invalid color {value!r}; use #RRGGBB or #RRGGBBAA")
    try:
        return tuple(
            int(normalized[index : index + 2], 16)
            for index in range(0, len(normalized), 2)
        )
    except ValueError as error:
        raise ValueError(f"invalid color {value!r}; use hexadecimal digits") from error


def load_branding(path: Path) -> dict[str, dict[str, str]]:
    try:
        content = json.loads(path.read_text(encoding="utf-8"))
        for section in ("dmg", "windows", "icon"):
            if not isinstance(content.get(section), dict):
                raise ValueError(f"missing object: {section}")
        return content
    except (OSError, json.JSONDecodeError, ValueError) as error:
        raise ValueError(f"invalid installer branding config {path}: {error}") from error


def load_icon(source_path: Path) -> Image.Image:
    if source_path.suffix.lower() == ".svg":
        sips = shutil.which("sips")
        if sips is None:
            raise ValueError("SVG sources require macOS sips; use a PNG source instead")
        with tempfile.TemporaryDirectory() as temp_dir:
            rendered_path = Path(temp_dir) / "app-icon.png"
            subprocess.run(
                [sips, "-s", "format", "png", "-z", "1024", "1024", str(source_path), "--out", str(rendered_path)],
                check=True,
                capture_output=True,
                text=True,
            )
            with Image.open(rendered_path) as rendered:
                return rendered.convert("RGBA")

    with Image.open(source_path) as loaded:
        if loaded.format != "PNG":
            raise ValueError("the source must be an SVG or PNG file")
        return loaded.convert("RGBA")


def vertical_gradient(
    size: tuple[int, int], top: tuple[int, int, int], bottom: tuple[int, int, int]
) -> Image.Image:
    width, height = size
    image = Image.new("RGB", size)
    pixels = image.load()
    for y in range(height):
        ratio = y / max(height - 1, 1)
        color = tuple(round(a + (b - a) * ratio) for a, b in zip(top, bottom))
        for x in range(width):
            pixels[x, y] = color
    return image


DMG_BACKGROUND_POINT_SIZE = (720, 440)
DMG_BACKGROUND_SUPERSAMPLE = 4


def draw_dmg_artwork(size: tuple[int, int], config: dict[str, str], scale: int) -> Image.Image:
    def s(value: float) -> float:
        return value * scale

    image = vertical_gradient(
        size, color(config["backgroundTop"]), color(config["backgroundBottom"])
    ).convert("RGBA")
    glow = Image.new("RGBA", image.size, (0, 0, 0, 0))
    glow_draw = ImageDraw.Draw(glow)
    glow_draw.ellipse((s(180), s(-190), s(700), s(330)), fill=color(config["glow"]))
    image.alpha_composite(glow.filter(ImageFilter.GaussianBlur(s(55))))

    draw = ImageDraw.Draw(image)
    draw.text(
        (s(360), s(54)),
        config["title"],
        anchor="mm",
        fill=color(config["titleColor"]),
        font=fitted_font(config["title"], s(620), round(s(27)), min_size=round(s(18)), bold=True),
    )
    draw.text(
        (s(360), s(87)),
        config["subtitle"],
        anchor="mm",
        fill=color(config["subtitleColor"]),
        font=fitted_font(config["subtitle"], s(620), round(s(16)), min_size=round(s(11))),
    )
    panel_bounds = (s(72), s(132), s(648), s(368))
    shadow = Image.new("RGBA", image.size, (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle(
        (s(72), s(140), s(648), s(376)), radius=s(32), fill=(32, 53, 89, 24)
    )
    image.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(s(18))))
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle(
        panel_bounds,
        radius=s(32),
        fill=color(config["panel"]),
        outline=color(config["panelOutline"]),
        width=round(s(2)),
    )
    draw.line((s(300), s(233), s(398), s(233)), fill=color(config["arrow"]), width=round(s(8)))
    draw.polygon(
        ((s(390), s(214)), (s(420), s(233)), (s(390), s(252))), fill=color(config["arrow"])
    )
    return image


def save_hidpi_tiff(frame_1x: Image.Image, frame_2x: Image.Image, destination: Path) -> None:
    """Pack a 1x/2x pair into an Apple multi-resolution TIFF.

    Finder reads a plain single-resolution PNG as 1x pixels and upscales it on
    Retina displays, which is what made past DMG backgrounds look blurry.
    ``tiffutil -cathidpicheck`` tags each frame with its point size so Finder
    picks the matching pixel density instead of stretching the low-res image.
    """
    tiffutil = shutil.which("tiffutil")
    if tiffutil is None:
        raise RuntimeError(
            "tiffutil was not found (it ships with macOS). Generate installer "
            "branding on macOS to produce a Retina-sharp dmg-background.tiff."
        )
    with tempfile.TemporaryDirectory() as raw_tmp_dir:
        tmp_dir = Path(raw_tmp_dir)
        path_1x = tmp_dir / "background.png"
        path_2x = tmp_dir / "background@2x.png"
        frame_1x.convert("RGB").save(path_1x, format="PNG")
        frame_2x.convert("RGB").save(path_2x, format="PNG")
        subprocess.run(
            [tiffutil, "-cathidpicheck", str(path_1x), str(path_2x), "-out", str(destination)],
            check=True,
            capture_output=True,
            text=True,
        )


def generate_dmg_background(destination: Path, config: dict[str, str]) -> None:
    width, height = DMG_BACKGROUND_POINT_SIZE
    master = draw_dmg_artwork(
        (width * DMG_BACKGROUND_SUPERSAMPLE, height * DMG_BACKGROUND_SUPERSAMPLE),
        config,
        DMG_BACKGROUND_SUPERSAMPLE,
    )
    frame_1x = master.resize((width, height), Image.Resampling.LANCZOS)
    frame_2x = master.resize((width * 2, height * 2), Image.Resampling.LANCZOS)
    save_hidpi_tiff(frame_1x, frame_2x, destination)


def generate_nsis_sidebar(
    source: Image.Image, destination: Path, config: dict[str, str]
) -> None:
    image = vertical_gradient(
        (164, 314), color(config["gradientTop"]), color(config["gradientBottom"])
    ).convert("RGBA")
    glow = Image.new("RGBA", image.size, (0, 0, 0, 0))
    glow_draw = ImageDraw.Draw(glow)
    glow_draw.ellipse((-62, 160, 210, 420), fill=color(config["glow"]))
    image.alpha_composite(glow.filter(ImageFilter.GaussianBlur(30)))
    image.alpha_composite(resized(source, 82), (41, 45))
    draw = ImageDraw.Draw(image)
    draw.text(
        (82, 244),
        config["eyebrow"],
        anchor="mm",
        fill=color(config["secondaryText"]),
        font=fitted_font(config["eyebrow"], 132, 13, min_size=8, bold=True),
    )
    draw.text(
        (82, 267),
        config["title"],
        anchor="mm",
        fill=color(config["primaryText"]),
        font=fitted_font(config["title"], 132, 24, min_size=12, bold=True),
    )
    image.convert("RGB").save(destination, format="BMP")


def generate_wix_artwork(
    source: Image.Image,
    dialog_path: Path,
    banner_path: Path,
    config: dict[str, str],
) -> None:
    dialog = Image.new("RGB", (493, 312), (255, 255, 255))
    panel = vertical_gradient(
        (164, 312), color(config["gradientTop"]), color(config["gradientBottom"])
    ).convert("RGBA")
    panel.alpha_composite(resized(source, 82), (41, 44))
    panel_draw = ImageDraw.Draw(panel)
    panel_draw.text(
        (82, 246),
        config["eyebrow"],
        anchor="mm",
        fill=color(config["secondaryText"]),
        font=fitted_font(config["eyebrow"], 132, 13, min_size=8, bold=True),
    )
    panel_draw.text(
        (82, 269),
        config["title"],
        anchor="mm",
        fill=color(config["primaryText"]),
        font=fitted_font(config["title"], 132, 24, min_size=12, bold=True),
    )
    dialog.paste(panel.convert("RGB"), (0, 0))
    dialog.save(dialog_path, format="BMP")

    banner = vertical_gradient(
        (493, 58), color(config["bannerTop"]), color(config["bannerBottom"])
    ).convert("RGBA")
    banner.alpha_composite(resized(source, 42), (438, 8))
    banner.convert("RGB").save(banner_path, format="BMP")


def main() -> int:
    args = parse_arguments()
    source_path = args.source.expanduser().resolve()
    config_path = args.config.expanduser().resolve()
    if not source_path.is_file():
        print(f"Icon source was not found: {source_path}", file=sys.stderr)
        return 1
    if not config_path.is_file():
        print(f"Branding configuration was not found: {config_path}", file=sys.stderr)
        return 1

    macos_path = ROOT / "deploy" / "backend" / "macos" / "icons" / "app.icns"
    windows_path = ROOT / "deploy" / "backend" / "windows" / "icons" / "app.ico"
    linux_path = ROOT / "deploy" / "backend" / "linux" / "icons" / "app.png"
    dmg_background_path = ROOT / "deploy" / "backend" / "macos" / "installer" / "dmg-background.tiff"
    nsis_sidebar_path = ROOT / "deploy" / "backend" / "windows" / "installer" / "nsis-sidebar.bmp"
    wix_dialog_path = ROOT / "deploy" / "backend" / "windows" / "installer" / "wix-dialog.bmp"
    wix_banner_path = ROOT / "deploy" / "backend" / "windows" / "installer" / "wix-banner.bmp"
    preview_path = ROOT / "deploy" / "assets" / "app-icon-preview.png"
    favicon_path = ROOT / "code" / "frontend" / "public" / "app-icon.png"
    apple_master_path = ROOT / "deploy" / "backend" / "macos" / "icons" / "app-icon-1024.png"
    if args.skip_dmg and not dmg_background_path.is_file():
        print("The committed DMG background is missing; generate it on macOS first.", file=sys.stderr)
        return 1
    outputs = (
        macos_path,
        windows_path,
        linux_path,
        dmg_background_path,
        nsis_sidebar_path,
        wix_dialog_path,
        wix_banner_path,
        preview_path,
        favicon_path,
        apple_master_path,
    )
    made_from = fingerprint(source_path, config_path)
    stamp = DEFAULT_STAMP.read_text(encoding="utf-8").strip() if DEFAULT_STAMP.is_file() else ""
    if not args.force and stamp == made_from and all(path.is_file() for path in outputs):
        # Up to date: nothing is written, and the outputs are as new as what
        # they were made from, so no build system asks for them again.
        for path in outputs:
            os.utime(path, None)
        print(f"Branding is up to date ({DEFAULT_STAMP.relative_to(ROOT)}); --force draws it again")
        return 0

    load_pillow()
    try:
        source = load_icon(source_path)
        if source.width != source.height:
            raise ValueError("the source must be square")
        if source.width < 512:
            raise ValueError("the source must be at least 512x512 pixels")
    except (OSError, ValueError) as error:
        print(f"Invalid icon source: {error}", file=sys.stderr)
        return 1

    try:
        branding = load_branding(config_path)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 1

    for path in outputs:
        path.parent.mkdir(parents=True, exist_ok=True)

    app_icon = foreground_icon(source)
    apple_master = vertical_gradient(
        (1024, 1024), color(branding["icon"]["backgroundTop"]),
        color(branding["icon"]["backgroundBottom"]),
    ).convert("RGBA")
    apple_master.alpha_composite(app_icon)
    # Icon Composer consumes a square, opaque, unmasked 1024-pixel layer.
    # ICNS below remains the export used by the current native bundle.
    apple_master.convert("RGB").save(
        apple_master_path, format="PNG", optimize=True, icc_profile=SRGB_PROFILE
    )
    app_icon.save(preview_path, format="PNG", optimize=True)
    resized(app_icon, 128).save(favicon_path, format="PNG", optimize=True)

    save_icns(macos_compat_icon(apple_master), macos_path)

    app_icon.save(
        windows_path,
        format="ICO",
        sizes=[(size, size) for size in (16, 24, 32, 48, 64, 128, 256)],
    )
    resized(app_icon, 256).save(linux_path, format="PNG", optimize=True)
    if not args.skip_dmg:
        generate_dmg_background(dmg_background_path, branding["dmg"])
    generate_nsis_sidebar(app_icon, nsis_sidebar_path, branding["windows"])
    generate_wix_artwork(
        app_icon, wix_dialog_path, wix_banner_path, branding["windows"]
    )

    print(f"macOS:  {macos_path}")
    print(f"Windows: {windows_path}")
    print(f"Linux:  {linux_path}")
    print(f"DMG:    {dmg_background_path}")
    print(f"NSIS:   {nsis_sidebar_path}")
    print(f"WiX:    {wix_dialog_path}, {wix_banner_path}")
    print(f"Preview: {preview_path}")
    print(f"Web:     {favicon_path}")
    print(f"Apple master: {apple_master_path}")
    if args.skip_dmg:
        # The DMG background was kept, not drawn: the set is whole only once a
        # Mac draws it, so the stamp is left for the Mac's next build.
        print(f"Not stamped: the DMG background is drawn on macOS ({DEFAULT_STAMP.relative_to(ROOT)})")
        return 0
    DEFAULT_STAMP.write_text(made_from + "\n", encoding="utf-8")
    print(f"Made from: {DEFAULT_STAMP.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
