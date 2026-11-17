# Manual ROM Integration Guide (`system`, `product`, `odm`, `super.img`)

This guide walks ROM porters through permanently integrating all Xiaomi 15 (`dada`) camera fixes into an EROFS-based ColorOS 17 `super.img` build.

---

## Prerequisites

- **Linux x86_64 host** with Python 3.10+, Pillow (`pip install pillow`), Java 17+, `zipalign`, `apksigner`, `apktool` (`>= 2.11.1`), `erofs-utils` (`mkfs.erofs`, `fsck.erofs`, `dump.erofs`), and `android-tools` (`lpmake`, `lpdump`, `lpunpack`).
- **Android NDK r26+** (if building `libdada_camfix.so` and `libdada_rtfix.so` from source).
- **Extracted ROM Trees** on a POSIX filesystem (`ext4`/`xfs`, so UID/GID/permissions/timestamps are preserved).
- **Target ROM / HAL Verification Note:** Hardcoded `.got` / JNI offsets in `dada_camfix.cpp` and `dada_rtfix.cpp` target **ColorOS 17 V6 CN (`OnePlus/PLZ110/OP64DDL1`)** `/system/lib64/libcameraservice.so` and `/system/lib64/libandroid_runtime.so`. Verify offsets with `llvm-objdump` / `readelf` before applying to a different ColorOS release.

---

## 1. Build and Inject Native `system` Shims

Compile `libdada_camfix.so` and `libdada_rtfix.so` (or extract the prebuilt `.so` files from GitHub Releases `v1.0.0`):

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk-r26d
cd native_hooks
./build.sh
```

Copy both shims into your staged `system` tree:

```bash
cp out/libdada_camfix.so /path/to/trees/system/system/lib64/libdada_camfix.so
cp out/libdada_rtfix.so  /path/to/trees/system/system/lib64/libdada_rtfix.so
chmod 0644 /path/to/trees/system/system/lib64/libdada_*.so
```

Inject `DT_NEEDED` into `libcameraservice.so` and `libandroid_runtime.so` using `patch_elf_needed.py`:

```bash
python3 native_hooks/patch_elf_needed.py \
  /path/to/orig_system/system/lib64/libcameraservice.so \
  /path/to/trees/system/system/lib64/libcameraservice.so \
  libdada_camfix.so

python3 native_hooks/patch_elf_needed.py \
  /path/to/orig_system/system/lib64/libandroid_runtime.so \
  /path/to/trees/system/system/lib64/libandroid_runtime.so \
  libdada_rtfix.so

chmod 0644 /path/to/trees/system/system/lib64/libcameraservice.so \
           /path/to/trees/system/system/lib64/libandroid_runtime.so
```

Verify with `readelf -d`:
```bash
readelf -d /path/to/trees/system/system/lib64/libcameraservice.so | grep NEEDED | head -n 5
readelf -d /path/to/trees/system/system/lib64/libandroid_runtime.so | grep NEEDED | head -n 5
```

Ensure `system` `file_contexts` and `fs_config` contain entries for the two new libraries:
```text
# file_contexts
/system/lib64/libdada_camfix\.so -- u:object_r:system_lib_file:s0
/system/lib64/libdada_rtfix\.so -- u:object_r:system_lib_file:s0

# fs_config
system/lib64/libdada_camfix.so 0 0 0644 capabilities=0x0
system/lib64/libdada_rtfix.so 0 0 0644 capabilities=0x0
```

---

## 2. Patch `MiuiCamera.apk` (`product`)

1. Decode your `MiuiCamera.apk` (`6.7.000180.0`):
   ```bash
   java -Xmx4g -jar apktool.jar d -f MiuiCamera.apk -o MiuiCamera_dec
   ```
2. Apply the DNG `SubIFD[0]` thumbnail smali patch (`n7/x.smali`):
   ```bash
   patch -d MiuiCamera_dec -p1 < patches/miuicamera_dng_thumbnail.patch
   ```
3. Extract `recbg.png`, `recfg.png`, and `monochrome.png` from ColorOS 17 `my_product` (see [icon_port/README.md](../icon_port/README.md)) and run:
   ```bash
   python3 icon_port/port_coloros_icon.py \
     --src-dir ./icon_src \
     --apk-dir ./MiuiCamera_dec
   ```
4. Rebuild, 16K-align, and sign (disable v4 so no `.idsig` is produced):
   ```bash
   java -Xmx4g -jar apktool.jar b MiuiCamera_dec -o MiuiCamera_unsigned.apk
   zipalign -f -P 16 4 MiuiCamera_unsigned.apk MiuiCamera_aligned.apk
   apksigner sign \
     --ks /path/to/rom_key.p12 \
     --ks-key-alias <alias> \
     --v1-signing-enabled false \
     --v2-signing-enabled true \
     --v3-signing-enabled true \
     --v4-signing-enabled false \
     --out /path/to/trees/product/priv-app/MiuiCamera/MiuiCamera.apk \
     MiuiCamera_aligned.apk
   rm -f /path/to/trees/product/priv-app/MiuiCamera/*.idsig
   chmod 0644 /path/to/trees/product/priv-app/MiuiCamera/MiuiCamera.apk
   ```

> **CRITICAL:** Never place `MiuiCamera.apk.idsig` in `/product/priv-app/MiuiCamera/`. `PackageManager` rejects v4 `.idsig` files under `/product` at boot.

---

## 3. Patch `/odm` Portrait Depth Configs & Properties

1. Extract stock `dada` `odm_a.img` to your POSIX workspace tree and apply both JSON patches:
   ```bash
   patch -d /path/to/trees/odm/etc/camera -p4 < patches/dualcam_predepth_params_pro.patch
   patch -d /path/to/trees/odm/etc/camera -p4 < patches/dualcam_depth_params_pro.patch
   chmod 0644 /path/to/trees/odm/etc/camera/dualcam_*.json
   ```
   *(If line endings differ, verify `"paramIsTrustAfCode": 0` in `dualcam_predepth_params_pro.json` and `"paramSDR": 0, "paramSDRT": 0.0` in `dualcam_depth_params_pro.json`.)*
2. Append the two persistent properties from `patches/odm_build_prop.txt` to `/path/to/trees/odm/etc/build.prop`:
   ```bash
   cat <<'EOF' >> /path/to/trees/odm/etc/build.prop

   # Xiaomi dada ColorOS 17 portrait depth tuning
   persist.vendor.camera.mialgo.capdepth.debuglitemode=0
   persist.vendor.camera.mialgo.capbokeh.debuglitemode=0
   EOF
   chmod 0644 /path/to/trees/odm/etc/build.prop
   ```
3. Ensure `odm` `file_contexts` labels `/odm/etc/build\.prop` and `/odm/etc/camera/.*` as `u:object_r:vendor_configs_file:s0` (`0644` `0 0`).

---

## 4. Rebuild EROFS Partitions and Pack `super.img`

Rebuild `product.img`, `system.img` (using `-Ededupe`), and `odm.img`:

```bash
mkfs.erofs -b4096 -zlz4hc,level=12 -C65536 -Ededupe -x2 -T1230768000 \
  --mkfs-time --preserve-mtime -U <product-uuid> --mount-point=/product \
  --fs-config-file=product_fs_config --file-contexts=product_file_contexts \
  out/product.img /path/to/trees/product

mkfs.erofs -b4096 -zlz4hc,level=12 -C65536 -Ededupe -x2 -T1230768000 \
  --mkfs-time --preserve-mtime -U <system-uuid> --mount-point=/ \
  --fs-config-file=system_fs_config --file-contexts=system_file_contexts \
  out/system.img /path/to/trees/system

mkfs.erofs -b4096 -zlz4hc,level=12 -C65536 -x2 -T1230768000 \
  --mkfs-time --preserve-mtime -U <odm-uuid> --mount-point=/odm \
  --fs-config-file=odm_fs_config --file-contexts=odm_file_contexts \
  out/odm.img /path/to/trees/odm
```

Verify each image with `fsck.erofs --extract <image>` and `dump.erofs`, then pack `super.img` (`11,811,160,064` bytes, `qti_dynamic_partitions_a` / `_b`) using `lpmake` and verify slot metadata with `lpdump --slot={0,1,2} out/super.img`.
