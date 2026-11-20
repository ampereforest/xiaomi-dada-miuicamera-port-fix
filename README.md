# Xiaomi 15 (`dada`) MiuiCamera Fixes for ColorOS 17 Ports

Permanent native framework shims, `MiuiCamera.apk` smali/adaptive-icon patches, and `/odm` Portrait depth tuning for running HyperOS `MiuiCamera` (`6.7.000180.0`) on **ColorOS 17 (`OnePlus/PLZ110/OP64DDL1`, Android 17 / API 37)** ported to **Xiaomi 15 (`dada`, `24129PN74G`)** with Xiaomi `OS4.0.0.11.XOCMIXM` / `OS4.0.0.22.XOCCNXM` ODM/HAL blobs.

> **Important Scope Notice:** This repository provides source code, unified patches, icon-porting tooling, and ROM partition integration guides for ROM porters (`system`, `product`, `odm`, and `super.img` repacking). `MiuiCamera.apk` and proprietary firmware images are **not** redistributed here.

---

## Feature & Fix Matrix

| Area | Symptom on Stock Port | Root Cause | Permanent Fix | Verified Status |
| :--- | :--- | :--- | :--- | :--- |
| **50MP Photo (`Ultra HD`)** | Cropped center output or failed `8192x6144` `BLOB` stream | ColorOS `libcameraservice.so` omits Xiaomi `0x81b90000` (`xiaomi.scaler.availableStreamConfigurations`) lookups in `getMaxJpegResolution` & `roundBufferDimensionNearest` | `libdada_camfix.so` (`DT_NEEDED` in `/system/lib64/libcameraservice.so`) resolves `8192x6144` `BLOB (33)` & `RAW_SENSOR (32)` | Full `6144x8192` / `8192x6144` uncropped JPEG |
| **Pro 50MP RAW & Ultra RAW** | Green/corrupt `8192x6144` DNG metadata mismatch & missing DNG preview thumbnail (`SubIFD[0]`) | (1) `DngCreator` lacks 2x full-res sensor array fallback & `Xiaomi Ultra RAW` 14-bit (`0..16383`) overrides; (2) ColorOS `libandroid_runtime.so` lacks HyperOS `DngCreator.writeJpeg(IILjava/nio/ByteBuffer;)V` | `libdada_rtfix.so` (`DT_NEEDED` in `/system/lib64/libandroid_runtime.so`) + `n7/x.smali` `embedPreviewThumb()` generating `256x192` `ARGB_8888` `setThumbnail()` | `8192x6144` 10-bit Pro RAW + `4096x3072` 14-bit Ultra RAW with crisp `256x192` `SubIFD[0]` |
| **Portrait Mode Bokeh (`1x` / `1.5x` / `2.6x`)** | Zero depth blur on the `1x` / `1.5x` Wide+UW path, or half-screen blur dropouts (Mono fallback) indoors; weak blur on non-human foreground objects | (1) `StructRear.paramD=64` + strict SGBM leave most of `sp_disp_map` empty at close range; (2) DenseNN near-depth prior + `useConstDR=1, dR=2.0/2.2`; (3) `StructMt.paramIuxT=400` forces indoor Dual→Mono fallback; (4) `StructCalDofRadius_Mono` attenuates non-face blur 4x–60x | Pure JSON tuning in `odm/etc/camera/` (`dualcam_depth`, `dualcam_predepth`, `dualcam_bokeh`); no `.so` or `MiuiCamera.apk` changes | Verified on a non-human scene: 3/3 consecutive `1x` `f/0.95` shots, lower-left light-bulb blur `BL` `0.03` → `0.63–1.28`. Human subjects not yet verified (see Known Limitations) |
| **Launcher Icon (Classic + Flux)** | Legacy HyperOS icon; missing ColorOS 17 `<adaptive-icon>` & `<monochrome>` layers | Ported `MiuiCamera.apk` shipped only legacy `.webp` icons; native `monochrome.png` (`92/180`) also scales smaller than ColorOS 17 Flux icons | `icon_port/port_coloros_icon.py` (`alpha_composite()`, `S_FG=0.96`, `S_MONO=1.524`) + `flb.xml` + `android:roundIcon="@mipmap/flb"` | Classic `halo/sq=0.914` (`center=(0,0)`); Flux `mono/sq=0.7812` (`+0.56%` vs native) |

---

## Portrait Mode Bokeh & Non-Human Subject Fix (Pure JSON)

A pure ODM configuration fix: three camera tuning files, with zero `.so` or `MiuiCamera.apk` binary changes.

### Root Causes

1. **Sparse close-range stereo disparity.** `StructRear.paramD=64` with strict SGBM thresholds (`paramU`, `paramH`, `paramI`, `paramSP`) and `StructBM.paramT=500` left most of `sp_disp_map.bmp` empty on close subjects. Stock baseline on a close-range non-human scene: `95.8%` empty at `1x` and `99.94%` empty at `1.5x`.
2. **DenseNN near-depth prior.** With `StructDenseNN.paramDeploySwitchA=0`, DenseNN propagated near depth into textureless lower-frame regions. `StructDofMethod` (`useConstDR=1`, `dRColse=2.0`, `dRFar=2.2`) kept those depth transitions sharp.
3. **Premature indoor Dual→Mono fallback.** `StructMt.paramIuxT=400` (`paramMtS=1`) switched to monocular `aiDepth` whenever `luxIdx > 400`, which covers normal indoor lighting.
4. **Non-human blur attenuation.** `StructCalDofRadius_Mono` `dof_0_*` (`12–15`) and `lut_linear_0_*` (`0.1–17`) attenuated non-face blur `4x–60x` relative to the face-blur indices.

### Changed Files

| File (`odm/etc/camera/`) | SHA-256 | Key changes vs. OEM |
| :--- | :--- | :--- |
| `dualcam_depth_params_pro.json` | `0ebbf6aabd541d908204d559ad349ae24d8f7c308fd245404aa2491a9692fb4c` | `StructB.{NORMAL,HOT,HARD}`: `StructRear.paramD` `64→96`; `StructSGBM.paramU` `30/20→12`, `paramH` `200→80`, `paramI` `16→8`, `paramSP` `1→0`; `StructBM.paramT` `500→150`. `StructI.paramT` `200→80`. `StructMt`: `paramMtS` `1→0`, `paramIuxT` `400→9999`. `StructPostProSwitch`: `paramSDR` `1→0`, `paramSDRT` `0.03→0.0`. |
| `dualcam_predepth_params_pro.json` | `254c690adf666dbf5d9a33929225d3a7bf646feb225f86b75759a6570c599882` | `StructDenseNN.paramDeploySwitchA` `0→1`; `paramIsTrustAfCode` `1→0`. |
| `dualcam_bokeh_params_pro.json` | `1d74901b49396024c458c50822287b25ece76733ab93fec952f45f105dd74bef` | `StructDofMethod`: `useConstDR` `1→0`, `dRColse` `2.0→0.8`, `dRFar` `2.2→0.8`. `StructCalDofRadius_Mono` (focal indices `0–4`): non-face `dof_0_*` `→2`, `lut_linear_0_*` `→25`, `lut_gamma_0_*` `→0.85`. |

### Verified Results

Verified on Xiaomi 15 (`dada`) running ColorOS 17, Portrait mode, with the production path (no debug properties set).

- **`1x` (`23mm`) `f/0.95`, 3 consecutive shots:** lower-left ceiling light-bulb blur `BL` rose from `0.03` (stock) to `1.28`, `1.12`, `0.63`. Overall blur-diff `ALL` was `2.63`, `2.44`, `2.42`.
- **`1.5x` (`35mm`) `f/0.95`, 1 shot:** `BL` `0.41` (stock) → `2.18`; `ALL` `1.94`.
- **`1x` (`23mm`) `f/2.8`, 1 shot:** `ALL` `1.26`, with aperture-dependent blur scaling.
- **Stereo coverage (pre-reboot ablation, `G2_d96`):** `1.5x` `sp_disp_map` non-empty pixels `0.06%` → `8.60%` (`143x`); `1x` `4.20%` → `11.20%`. Post-reboot coverage was not measured.
- **Edge quality (1:1 unscaled crops):** clean edges on a non-human box (cardboard fibre, printed lettering, insect antennae), with no halo or foreground blur bleed.
- **Stability:** no crashes and no `dualcam_*` SELinux denials during the verification run.

### Applying

Option A, copy the tuned files into an ODM build that matches the OEM version:

```bash
cp odm/etc/camera/dualcam_*_params_pro.json <odm>/etc/camera/
```

Option B, patch an OEM ODM. The on-device files use CRLF line endings, so normalize them to LF first. The patches expect LF.

```bash
# from the directory that contains odm/
for f in depth predepth bokeh; do
  sed -i 's/\r$//' odm/etc/camera/dualcam_${f}_params_pro.json
  patch -p1 < patches/dualcam_${f}_params_pro.patch
done
```

Applying all three patches to LF-normalized OEM files reproduces the three SHA-256 values above. Set the file mode to `0644 root:root` with label `u:object_r:vendor_configs_file:s0`.

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
├── odm/etc/camera/
│   ├── dualcam_depth_params_pro.json     # Tuned Portrait stereo depth (Checkpoint 71)
│   ├── dualcam_predepth_params_pro.json  # Tuned DenseNN switch & AF trust
│   └── dualcam_bokeh_params_pro.json     # Tuned DoF method & non-human Mono blur LUT
├── patches/
│   ├── miuicamera_dng_thumbnail.patch    # Unified diff for smali/n7/x.smali
│   ├── miuicamera_adaptive_icon.patch    # Unified diff for AndroidManifest.xml + flb.xml
│   ├── dualcam_predepth_params_pro.patch # DenseNN switch & paramIsTrustAfCode = 0
│   ├── dualcam_depth_params_pro.patch    # Stereo SGBM/BM/Mt tuning, paramSDR = 0, paramSDRT = 0.0
│   ├── dualcam_bokeh_params_pro.patch    # DoF method & non-human Mono blur LUT
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
3. **Apply ODM Portrait Configs & Props (`odm`):** Copy `odm/etc/camera/dualcam_*_params_pro.json` (or apply the three `patches/dualcam_*_params_pro.patch` files to LF-normalized OEM files, see [Applying](#applying)), and append `patches/odm_build_prop.txt` to `/odm/etc/build.prop`.
4. **Rebuild EROFS & Pack `super.img`:** Follow [docs/MANUAL_INTEGRATION_GUIDE.md](docs/MANUAL_INTEGRATION_GUIDE.md#4-rebuild-erofs-partitions-and-pack-superimg).

---

## Known Limitations (Future Work)

1. **Portrait Human-Subject Verification:** The Portrait fix above is verified on non-human subjects (box, desk, light bulb). Human-subject Portrait captures were not part of the recorded verification run, so human edge quality and `f/0.95` blur on people remain to be confirmed. Non-human `1x` `f/0.95` light-bulb blur also varies between shots (`BL` `0.63–1.28`).
2. **50MP 14-bit Ultra RAW:** Pro 50MP RAW works at `8192x6144` (`10-bit`, `WhiteLevel=1023`), and Ultra RAW works at `12.5MP` (`4096x3072`, `14-bit`, `WhiteLevel=16383`). Full `8192x6144` 14-bit Ultra RAW remains unimplemented.

---

## Credits

- **`turbo-boo/android_device_xiaomi_dada-miuicamera`** — Initial `DngCreator` / `8192x6144` full-resolution RAW metadata research for Xiaomi 15 (`dada`).
- Xiaomi `dada` HyperOS (`OS4.0.0.11.XOCMIXM` / `OS4.0.0.22.XOCCNXM`) & ColorOS 17 (`OnePlus/PLZ110/OP64DDL1`) port engineering contributors.
