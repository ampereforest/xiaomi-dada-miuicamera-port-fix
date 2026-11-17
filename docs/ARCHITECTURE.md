# Technical Architecture & Root Cause Analysis

Target Stack:
- **Device:** Xiaomi 15 (`dada`, model `24129PN74G`)
- **Framework / System / Product Base:** ColorOS 17 V6 CN (`OnePlus/PLZ110/OP64DDL1`, Android 17 / API 37)
- **Vendor / ODM Camera Stack:** Xiaomi `dada` `OS4.0.0.11.XOCMIXM` / `OS4.0.0.22.XOCCN` (`camx`, `MIVI2`, `AISP`, `SNPE`/`QNN` CDSP depth models)
- **Camera App:** HyperOS `MiuiCamera` (`com.android.camera`, version `6.7.000180.0`)

---

## 1. 50MP Photo (`8192x6144`) — `libdada_camfix.so` -> `libcameraservice.so`

### Root Cause
Xiaomi's Camera HAL exposes standard `12MP` (`4096x3072`) stream configurations under `android.scaler.availableStreamConfigurations`, while full `50MP` (`8192x6144`) `BLOB (33)` and `RAW_SENSOR (32)` configurations are advertised in Xiaomi's vendor metadata tag `0x81b90000` (`xiaomi.scaler.availableStreamConfigurations`). Stock ColorOS `libcameraservice.so` does not inspect `0x81b90000` in:
- `getMaxJpegResolution(const CameraMetadata& info, bool isUltraHighResolution)` (GOT `0x523890`)
- `roundBufferDimensionNearest(..., bool maxResolutionMode, ...)` (GOT `0x525240`)

As a result, `50MP` captures fell back to `12MP` buffer dimensions (`4096x3072`) or mismatched HAL `CameraBlob` trailer sizes, producing cropped/corrupted `50MP` outputs.

### Fix (`native_hooks/dada_camfix.cpp`)
Compiled as `/system/lib64/libdada_camfix.so` and loaded via an injected `DT_NEEDED` entry in `libcameraservice.so`:
1. Guards execution with `ro.product.device == "dada"`.
2. Patches the `.got` slots for `getMaxJpegResolution` and `roundBufferDimensionNearest` via `mprotect()` + `__sync_synchronize()`.
3. Calls `CameraMetadata::find(0x81b90000)` (`GOT 0x522740` / `dlsym` fallback):
   - In `hook_getMaxJpegResolution` (when `isUltraHighResolution == true`), locates the matching `2x` `FMT_BLOB` entry (`8192x6144`) so the JPEG buffer matches the HAL `CameraBlob` trailer.
   - In `hook_roundBufferDimensionNearest`, preserves exact `(width, height)` for `FMT_RAW_SENSOR (32)` and `FMT_BLOB (33)` entries present in `0x81b90000`.

---

## 2. Pro 50MP RAW, Ultra RAW & DNG Preview Thumbnails (`libdada_rtfix.so` + `n7/x.smali`)

*Credit: Initial `DngCreator` / `8192x6144` full-res RAW metadata research by `turbo-boo/android_device_xiaomi_dada-miuicamera`.*

### Root Cause A — Native `DngCreator` Metadata
Stock ColorOS `/system/lib64/libandroid_runtime.so` populates `DngCreator` TIFF/EXIF tags from static `CameraCharacteristics`:
1. In `50MP` RAW (`8192x6144`), `SENSOR_INFO_PIXEL_ARRAY_SIZE`, `ACTIVE_ARRAY_SIZE`, and `PRE_CORRECTION_ACTIVE_ARRAY_SIZE` report `4096x3072`, causing `DngCreator` to fail or miswrite dimensions/CFA offsets for `8192x6144` buffers.
2. In `Xiaomi Ultra RAW` (`4096x3072` 14-bit linear/scaled Bayer), `DngCreator` needs `WhiteLevel = 16383`, `BlackLevelPattern = {0, 0, 0, 0}`, and `DynamicBlackLevel = {0, 0, 0, 0}`.

### Fix A (`native_hooks/dada_rtfix.cpp`)
Injected via `DT_NEEDED` into `/system/lib64/libandroid_runtime.so`:
1. Intercepts JNI function table slots for `DngCreator.nativeWriteImage` (`0x2d0320`) and `DngCreator.nativeWriteInputStream` (`0x2d0338`).
2. Reads `NativeContext` fields (`+0x90` characteristics, `+0xa0` capture result, `+0xbc` hasDescription, `+0xc0` description string).
3. Uses a `ScopedDngMetadataOverride` RAII guard with `libcamera_client.so` `CameraMetadata` copy/update/dtor symbols to temporarily double the array/active-rect tags when `(w, h) == 2x (pw, ph)` and override `WhiteLevel`/`BlackLevel` when `description == "Xiaomi Ultra RAW"`, cleanly restoring pointers on return.

### Root Cause B — Missing DNG `SubIFD[0]` Thumbnail
HyperOS `MiuiCamera` (`n7/x.smali`) embeds a JPEG preview into the `.dng` by calling a Xiaomi-only extension method `DngCreator.writeJpeg(IILjava/nio/ByteBuffer;)V`. ColorOS `libandroid_runtime.so` does not implement `writeJpeg`, so `n7/x.smali` threw `NoSuchMethodException` and emitted `.dng` files lacking `SubIFDs` (`330`). Gallery viewers then rendered low-resolution blurred placeholders or black tiles.

### Fix B (`patches/miuicamera_dng_thumbnail.patch`)
Adds `Ln7/x;->embedPreviewThumb(Landroid/hardware/camera2/DngCreator;Ljava/nio/ByteBuffer;)V`:
- Decodes the captured preview JPEG buffer (`this.q`) downsampled (`<= 512px`), scales proportionally to `<= 256px` (`256x192` for 4:3) `ARGB_8888`, and calls standard Android `DngCreator.setThumbnail(Bitmap)` prior to `writeInputStream()`.

---

## 3. Indoor Portrait Bokeh Fixes (`/odm` Configs & Properties)

### Root Cause 1 — `JudgeDistanceOverflow` `AfCode` Zero-Out
Indoor Portrait shots frequently arrive at `MialgoBokehDepthPreLaunch` (`libmialgo_bokeh_depth.so`) with `afState = 0` and uncalibrated `AfCode` values (`13..20`), triggering `JudgeDistanceOverflow` when `"paramIsTrustAfCode": 1` in `/odm/etc/camera/dualcam_predepth_params_pro.json`. That gate zeroes the depth buffers and emits empty `~15 KB` depth trailers.

### Root Cause 2 — Half-Screen / Blank-Wall `Mono` Fallback
Even with `paramIsTrustAfCode = 0`, two ODM gates forced indoor `1x` (`23mm`) captures with low-texture white walls out of Dual Stereo + DenseNN (`dualcam_caputre_h768w1024_quan16`) into the legacy 8-bit monocular model (`dualcam_capture_mono_quan8.minn`), which often misclassified blank walls onto the foreground plane:
1. **`StructPostProSwitch.paramSDR` (`dualcam_depth_params_pro.json`):** `paramSDR = 1` (`paramSDRT = 0.03`) aborted `DenseNN` (`Sp disp Fail`) whenever `< 3%` of pixels had sparse stereo disparity. Setting `"paramSDR": 0` and `"paramSDRT": 0.0` lets `AiDepthCompletionProcess` (`DenseNN`) run directly.
2. **Thermal `lite` Mode (`com.xiaomi.plugin.capdepth.so`, `midualcamallinone.so`):** Above `ThermalLevel >= 2` (`~35.4°C`), plugins set `lite = 1`, pushing `libmialgo_bokeh_depth.so` into `Mono` fallback unless overridden by:
   - `persist.vendor.camera.mialgo.capdepth.debuglitemode=0`
   - `persist.vendor.camera.mialgo.capbokeh.debuglitemode=0`

Adding these two properties to `/odm/etc/build.prop` ensures they persist across factory resets / data formats.

---

## 4. Native ColorOS 17 Adaptive + Flux Monochrome Icon (`icon_port/`)

ColorOS 17's launcher (`OplusLauncher`) renders native system icons from `my_product/media/theme/uxicons/hdpi/com.oplus.camera/{recbg.png,recfg.png,monochrome.png}` (`180x180`) using custom proportions, while third-party/ported APKs rely on `AdaptiveIconDrawable` (`108dp` canvas / `72dp` viewport) and `getMonochrome()` for Flux (`Match wallpaper color`) theming:
1. **Straight-Alpha Fix:** Standard Pillow `Image.paste(fg, box, fg)` onto transparent `(0, 0, 0, 0)` multiplies semi-transparent edge/bezel pixels against black and squares alpha, turning the luminous outer bezel (`alpha ~ 178`) dark grey. Using `alpha_composite()` preserves straight RGBA values.
2. **Calibrated Scales:**
   - `S_FG = 0.96` trims the outer warm bezel glow (`~0.92` of squircle) so it sits cleanly inside the ColorOS squircle mask.
   - `S_MONO = 1.524` compensates for `monochrome.png`'s `92/180` (`51.1%`) native opaque bounding box so the Flux glyph renders at `~0.78` of squircle width (matching native ColorOS 17 Flux proportions with zero opaque pixel clipping).
3. **APK v4 `.idsig` Warning:** `PackageManager` fails v4 certificate collection on boot if a `.apk.idsig` sidecar is placed in `/product/priv-app/MiuiCamera/`. Always sign with `--v4-signing-enabled false` and never ship `.idsig`.
