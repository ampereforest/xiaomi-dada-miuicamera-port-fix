#!/usr/bin/env python3
"""Port calibrated native ColorOS 17 Camera adaptive + monochrome icon into decoded MiuiCamera."""

import argparse
import os
import re
from PIL import Image

# Calibrated multipliers for ColorOS 17 OplusLauncher:
# - S_FG = 0.96 trims the outer bezel glow slightly inward for crisp squircle breathing room (~0.92 of squircle).
# - S_MONO = 1.524 scales monochrome.png (92/180 native bbox) to match native ColorOS Flux proportions (~0.78 of squircle).
S_FG = 0.96
S_MONO = 1.524

ADAPTIVE_SIZES = {
    "mipmap-xhdpi": (216, 144),    # 2x (108dp canvas / 72dp viewport)
    "mipmap-xxhdpi": (324, 216),   # 3x
    "mipmap-xxxhdpi": (432, 288),  # 4x
}

LEGACY_SIZES = {
    "mipmap-xhdpi": 235,
    "mipmap-xxhdpi": 470,
    "mipmap-xxxhdpi": 705,
}


def place_centered_rgba(canvas_sz: int, layer: Image.Image, target_sz: int) -> Image.Image:
    """Resize RGBA layer to target_sz and center on transparent canvas preserving straight alpha."""
    canvas = Image.new("RGBA", (canvas_sz, canvas_sz), (0, 0, 0, 0))
    scaled = layer.resize((target_sz, target_sz), Image.Resampling.LANCZOS)
    off = (canvas_sz - target_sz) // 2
    if off >= 0:
        canvas.alpha_composite(scaled, (off, off))
    else:
        crop_off = -off
        sb = scaled.getbbox()
        assert (
            sb
            and sb[0] >= crop_off
            and sb[1] >= crop_off
            and sb[2] <= crop_off + canvas_sz
            and sb[3] <= crop_off + canvas_sz
        ), f"Non-zero alpha pixels clipped: bbox={sb}, crop=[{crop_off}, {crop_off + canvas_sz})"
        cropped = scaled.crop((crop_off, crop_off, crop_off + canvas_sz, crop_off + canvas_sz))
        canvas.alpha_composite(cropped, (0, 0))
    return canvas


def port_icons(src_dir: str, apk_dir: str, s_fg: float = S_FG, s_mono: float = S_MONO) -> None:
    recbg = Image.open(os.path.join(src_dir, "recbg.png")).convert("RGBA")
    recfg = Image.open(os.path.join(src_dir, "recfg.png")).convert("RGBA")
    mono = Image.open(os.path.join(src_dir, "monochrome.png")).convert("RGBA")

    res_dir = os.path.join(apk_dir, "res")
    manifest = os.path.join(apk_dir, "AndroidManifest.xml")

    for folder, (canvas_sz, inner_sz) in ADAPTIVE_SIZES.items():
        out_dir = os.path.join(res_dir, folder)
        os.makedirs(out_dir, exist_ok=True)

        bg_im = recbg.resize((canvas_sz, canvas_sz), Image.Resampling.LANCZOS)
        bg_im.save(os.path.join(out_dir, "flb_bg.png"), "PNG")

        fg_canvas = place_centered_rgba(canvas_sz, recfg, round(inner_sz * s_fg))
        fg_canvas.save(os.path.join(out_dir, "flb_fg.png"), "PNG")

        mo_canvas = place_centered_rgba(canvas_sz, mono, round(inner_sz * s_mono))
        mo_canvas.save(os.path.join(out_dir, "flb_mono.png"), "PNG")

        lsz = LEGACY_SIZES[folder]
        leg_bg = recbg.resize((lsz, lsz), Image.Resampling.LANCZOS)
        leg_fg_sz = round(lsz * s_fg)
        leg_fg = recfg.resize((leg_fg_sz, leg_fg_sz), Image.Resampling.LANCZOS)
        leg_off = (lsz - leg_fg_sz) // 2
        leg_bg.alpha_composite(leg_fg, (leg_off, leg_off))
        leg_bg.convert("RGB").save(os.path.join(out_dir, "flb.webp"), "WEBP", lossless=True)

    anydpi_dir = os.path.join(res_dir, "mipmap-anydpi-v26")
    os.makedirs(anydpi_dir, exist_ok=True)
    flb_xml = os.path.join(os.path.dirname(__file__), "flb.xml")
    with open(flb_xml, "r", encoding="utf-8") as src_f:
        xml_content = src_f.read()
    with open(os.path.join(anydpi_dir, "flb.xml"), "w", encoding="utf-8") as dst_f:
        dst_f.write(xml_content)

    with open(manifest, "r", encoding="utf-8") as f:
        mf = f.read()
    if 'android:roundIcon="@mipmap/flb"' not in mf:
        mf_new, cnt = re.subn(
            r'(android:icon="@mipmap/flb")',
            r'\1 android:roundIcon="@mipmap/flb"',
            mf,
            count=1,
        )
        assert cnt == 1, "Failed to locate android:icon=\"@mipmap/flb\" in AndroidManifest.xml"
        with open(manifest, "w", encoding="utf-8") as f:
            f.write(mf_new)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--src-dir", required=True, help="Directory with recbg.png, recfg.png, monochrome.png")
    parser.add_argument("--apk-dir", required=True, help="Decoded MiuiCamera apktool directory")
    parser.add_argument("--s-fg", type=float, default=S_FG, help="Foreground scale multiplier (default: 0.96)")
    parser.add_argument("--s-mono", type=float, default=S_MONO, help="Monochrome scale multiplier (default: 1.524)")
    args = parser.parse_args()
    port_icons(args.src_dir, args.apk_dir, args.s_fg, args.s_mono)
    print(f"Ported ColorOS 17 Camera icons into {args.apk_dir} (S_FG={args.s_fg}, S_MONO={args.s_mono})")


if __name__ == "__main__":
    main()
