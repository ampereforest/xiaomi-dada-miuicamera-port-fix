# Xiaomi 15 (`dada`) MiuiCamera Fixes for ColorOS 17 Ports

Permanent native framework shims, `MiuiCamera.apk` smali/adaptive-icon patches, and `/odm` Portrait depth tuning for running HyperOS `MiuiCamera` (`6.7.000180.0`) on **ColorOS 17 (`OnePlus/PLZ110/OP64DDL1`, Android 17 / API 37)** ported to **Xiaomi 15 (`dada`, `24129PN74G`)** with Xiaomi `OS4.0.0.11.XOCMIXM` / `OS4.0.0.22.XOCCNXM` ODM/HAL blobs.

> **Important Scope Notice:** This repository provides source code, unified patches, icon-porting tooling, and ROM partition integration guides for ROM porters (`system`, `product`, `odm`, and `super.img` repacking). `MiuiCamera.apk` and proprietary firmware images are **not** redistributed here.

---

## Feature & Fix Matrix

| Area | Symptom on Stock Port | Root Cause | Permanent Fix | Verified Status |
| :--- | :--- | :--- | :--- | :--- |
| **50MP Photo (`Ultra HD`)** | Cropped center output or failed `8192x6144` `BLOB` stream | ColorOS `libcameraservice.so` omits Xiaomi `0x81b90000` (`xiaomi.scaler.availableStreamConfigurations`) lookups in `getMaxJpegResolution` & `roundBufferDimensionNearest` | `libdada_camfix.so` (`DT_NEEDED` in `/system/lib64/libcameraservice.so`) resolves `8192x6144` `BLOB (33)` & `RAW_SENSOR (32)` | Full `6144x8192` / `8192x6144` uncropped JPEG |
| **Pro 50MP RAW & Ultra RAW** | Green/corrupt `8192x6144` DNG metadata mismatch & missing DNG preview thumbnail (`SubIFD[0]`) | (1) `DngCreator` lacks 2x full-res sensor array fallback & `Xiaomi Ultra RAW` 14-bit (`0..16383`) overrides; (2) ColorOS `libandroid_runtime.so` lacks HyperOS `DngCreator.writeJpeg(IILjava/nio/ByteBuffer;)V` | `libdada_rtfix.so` (`DT_NEEDED` in `/system/lib64/libandroid_runtime.so`) + `n7/x.smali` `embedPreviewThumb()` generating `256x192` `ARGB_8888` `setThumbnail()` | `8192x6144` 10-bit Pro RAW + `4096x3072` 14-bit Ultra RAW with crisp `256x192` `SubIFD[0]` |
| **Portrait Mode Bokeh (`1x` / `1.5x` / `2.6x`)** | Zero depth blur (`15 KB` empty trailer) or half-screen/blank-wall blur dropouts (`Mono` fallback) indoors | (1) `JudgeDistanceOverflow` rejects uncalibrated `AfCode` (`13..20`, `afState=0`) when `paramIsTrustAfCode=1`; (2) `SpDensityRefine` (`paramSDR=1`) & thermal `lite=1` fallback to `dualcam_capture_mono_quan8.minn` | `dualcam_predepth_params_pro.json` (`paramIsTrustAfCode=0`), `dualcam_depth_params_pro.json` (`paramSDR=0, paramSDRT=0.0`), and `/odm/etc/build.prop` (`debuglitemode=0`) | Consistent DenseNN depth output across indoor low-texture scenes |
| **Launcher Icon (Classic + Flux)** | Legacy HyperOS icon; missing ColorOS 17 `<adaptive-icon>` & `<monochrome>` layers | Ported `MiuiCamera.apk` shipped only legacy `.webp` icons; native `monochrome.png` (`92/180`) also scales smaller than ColorOS 17 Flux icons | `icon_port/port_coloros_icon.py` (`alpha_composite()`, `S_FG=0.96`, `S_MONO=1.524`) + `flb.xml` + `android:roundIcon="@mipmap/flb"` | Classic `halo/sq=0.914` (`center=(0,0)`); Flux `mono/sq=0.7812` (`+0.56%` vs native) |

---

## Repository Layout

```text
├── README.md
├── LICENSE
├── docs/
│   ├── ARCHITECTURE.md               # Deep technical analysis of all 4 root causes
│   └── MANUAL_INTEGRATION_GUIDE.md   # End-to-end ROM build & super.img integration guide
├── native_hooks/
│   ├── dada_camfix.cpp               # libcameraservice.so GOT shim (50MP BLOB/RAW sizing)
│   ├── dada_rtfix.cpp                # libandroid_runtime.so DngCreator JNI hook (50MP RAW + Ultra RAW)
│   ├── patch_elf_needed.py           # Clean AArch64 ELF64 PT_LOAD + DT_NEEDED injector
│   └── build.sh                      # Android NDK r26+ C++ build script
├── patches/
│   ├── miuicamera_dng_thumbnail.patch    # Unified diff for smali/n7/x.smali
│   ├── miuicamera_adaptive_icon.patch    # Unified diff for AndroidManifest.xml + flb.xml
│   ├── dualcam_predepth_params_pro.patch # Sets paramIsTrustAfCode = 0
│   ├── dualcam_depth_params_pro.patch    # Sets paramSDR = 0, paramSDRT = 0.0
│   └── odm_build_prop.txt                # Persistent capdepth/capbokeh debuglitemode=0 props
└── icon_port/
    ├── port_coloros_icon.py          # Straight-alpha adaptive + Flux monochrome generator
    ├── flb.xml                       # <adaptive-icon> template with <monochrome>
    └── README.md                     # Instructions to extract native ColorOS 17 layers
```

---

## Quick Start for ROM Porters

1. **Compile & Inject Native Shims (`system`):** See [docs/MANUAL_INTEGRATION_GUIDE.md](docs/MANUAL_INTEGRATION_GUIDE.md#1-build-and-inject-native-system-shims) or download the prebuilt `libdada_camfix.so` and `libdada_rtfix.so` from [GitHub Releases (`v1.0.0`)](../../releases).
2. **Patch & Rebuild `MiuiCamera.apk` (`product`):** Apply `patches/miuicamera_dng_thumbnail.patch`, port ColorOS 17 icons with `icon_port/port_coloros_icon.py`, 16K-align (`zipalign -P 16 4`), sign v2+v3 (never ship `.apk.idsig` inside `/product/priv-app/MiuiCamera/`).
3. **Apply ODM Portrait Configs & Props (`odm`):** Apply `patches/dualcam_predepth_params_pro.patch` and `patches/dualcam_depth_params_pro.patch`, and append `patches/odm_build_prop.txt` to `/odm/etc/build.prop`.
4. **Rebuild EROFS & Pack `super.img`:** Follow [docs/MANUAL_INTEGRATION_GUIDE.md](docs/MANUAL_INTEGRATION_GUIDE.md#4-rebuild-erofs-partitions-and-pack-superimg).

---

## Known Limitations (Future Work)

1. **Portrait Non-Human Object Edge Segmentation & `f/0.95` DoF Strength:** While `paramSDR=0` + `debuglitemode=0` eliminates the zero-blur and half-screen `Mono` fallback bugs, Xiaomi's capture pipeline (`capdepth` `DenseNN` + `capbokeh`) relies heavily on human segmentation (`algo_head_seg` / `aio_seg`) and still produces softer edges and lighter `f/0.95` blur on non-human foreground objects than the live viewfinder (`anc-previewbokeh`).
2. **50MP 14-bit Ultra RAW:** Pro 50MP RAW works at `8192x6144` (`10-bit`, `WhiteLevel=1023`), and Ultra RAW works at `12.5MP` (`4096x3072`, `14-bit`, `WhiteLevel=16383`). Full `8192x6144` 14-bit Ultra RAW remains unimplemented.

---

## Credits

- **`turbo-boo/android_device_xiaomi_dada-miuicamera`** — Initial `DngCreator` / `8192x6144` full-resolution RAW metadata research for Xiaomi 15 (`dada`).
- Xiaomi `dada` HyperOS (`OS4.0.0.11.XOCMIXM` / `OS4.0.0.22.XOCCNXM`) & ColorOS 17 (`OnePlus/PLZ110/OP64DDL1`) port engineering contributors.
