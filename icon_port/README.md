# ColorOS 17 Adaptive & Flux Monochrome Icon Port

Because stock ColorOS 17 icon artwork belongs to the ColorOS firmware dump, PNG assets are not redistributed in this repository. Extract the three source layers directly from a ColorOS 17 firmware dump (`PLZ110_17.0.0.107(CN01)` or matching `my_product` partition) and run `port_coloros_icon.py`.

## 1. Extract Source Layers from ColorOS 17 `my_product.img`

```bash
mkdir -p /tmp/cos_myprod icon_src
sudo mount -o ro,loop /path/to/PLZ110_my_product.img /tmp/cos_myprod
cp /tmp/cos_myprod/media/theme/uxicons/hdpi/com.oplus.camera/{recbg.png,recfg.png,monochrome.png} icon_src/
sudo umount /tmp/cos_myprod
```

Expected files in `icon_src/`:
- `recbg.png` (`180x180`, silver-blue gradient background)
- `recfg.png` (`180x180`, 3D camera lens + orange accent dot)
- `monochrome.png` (`180x180`, alpha-channel monochrome lens silhouette)

## 2. Generate Adaptive Layers Into Decoded `MiuiCamera.apk`

```bash
python3 port_coloros_icon.py --src-dir ./icon_src --apk-dir /path/to/MiuiCamera_decoded
```

### Technical Notes
- **Straight-Alpha Preservation:** Uses Pillow `alpha_composite()` instead of `Image.paste(..., mask=)` so semi-transparent pixels in `recfg.png` (`alpha ~ 178`) are never premultiplied against black (which would turn the outer bezel ring dark grey).
- **Calibrated Scales:** `S_FG = 0.96` keeps crisp squircle margins in Classic mode (`~0.92` outer halo ratio), and `S_MONO = 1.524` scales `monochrome.png` (`92/180` native opaque bounding box) so Flux (`Match wallpaper color`) matches native ColorOS 17 system icons (`~0.78` ratio) with zero opaque pixel clipping.
- **Manifest & XML:** Copies `flb.xml` to `res/mipmap-anydpi-v26/flb.xml` and adds `android:roundIcon="@mipmap/flb"` alongside `android:icon="@mipmap/flb"`.
