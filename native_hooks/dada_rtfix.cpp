#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <link.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/system_properties.h>
#include <unistd.h>

#define LOG_TAG "DadaRtFix"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

constexpr uint32_t TAG_SENSOR_BLACK_LEVEL_PATTERN = 0x000e000cu;
constexpr uint32_t TAG_SENSOR_DYNAMIC_BLACK_LEVEL = 0x000e001cu;
constexpr uint32_t TAG_SENSOR_INFO_ACTIVE_ARRAY_SIZE = 0x000f0000u;
constexpr uint32_t TAG_SENSOR_INFO_PIXEL_ARRAY_SIZE = 0x000f0006u;
constexpr uint32_t TAG_SENSOR_INFO_WHITE_LEVEL = 0x000f0007u;
constexpr uint32_t TAG_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE = 0x000f000au;

// Offsets from ColorOS 17 V6 /system/lib64/libandroid_runtime.so
constexpr uintptr_t OFF_NATIVE_WRITE_IMAGE = 0x25628c;
constexpr uintptr_t OFF_NATIVE_WRITE_INPUT_STREAM = 0x2568fc;
constexpr uintptr_t OFF_JNI_WRITE_IMAGE_PTR = 0x2d0320;
constexpr uintptr_t OFF_JNI_WRITE_STREAM_PTR = 0x2d0338;

constexpr size_t OFF_CTX_CHARACTERISTICS = 0x90;
constexpr size_t OFF_CTX_RESULT = 0xa0;
constexpr size_t OFF_CTX_HAS_DESCRIPTION = 0xbc;
constexpr size_t OFF_CTX_DESCRIPTION = 0xc0;

struct RawCameraMetadata {
    void* mBuffer;
    bool mLocked;
    uint8_t pad[7];
};

struct CameraMetadataRoEntry {
    size_t index;
    uint32_t tag;
    uint8_t type;
    size_t count;
    union {
        const uint8_t* u8;
        const int32_t* i32;
        const float* f;
        const int64_t* i64;
        const double* d;
        const void* r;
    } data;
};

using CopyCtorFn = void (*)(RawCameraMetadata* self, const RawCameraMetadata* other);
using DtorFn = void (*)(RawCameraMetadata* self);
using FindConstFn = CameraMetadataRoEntry (*)(const RawCameraMetadata* self, uint32_t tag);
using UpdateInt32Fn = int32_t (*)(RawCameraMetadata* self, uint32_t tag, const int32_t* data, size_t count);
using UpdateFloatFn = int32_t (*)(RawCameraMetadata* self, uint32_t tag, const float* data, size_t count);

using WriteImageFn = void (*)(JNIEnv*, jobject, jobject, jint, jint, jobject, jint, jint, jlong, jboolean);
using WriteInputStreamFn = void (*)(JNIEnv*, jobject, jobject, jobject, jint, jint, jlong);

static CopyCtorFn g_copy_ctor = nullptr;
static DtorFn g_dtor = nullptr;
static FindConstFn g_find_const = nullptr;
static UpdateInt32Fn g_update_i32 = nullptr;
static UpdateFloatFn g_update_f = nullptr;

static WriteImageFn g_orig_write_image = nullptr;
static WriteInputStreamFn g_orig_write_stream = nullptr;
static jfieldID g_native_ctx_fid = nullptr;

static bool is_dada_device() {
    char prop[PROP_VALUE_MAX] = {0};
    __system_property_get("ro.product.device", prop);
    return strcmp(prop, "dada") == 0;
}

static bool resolve_camera_metadata_syms() {
    if (g_copy_ctor && g_dtor && g_find_const && g_update_i32 && g_update_f) {
        return true;
    }
    void* handle = dlopen("libcamera_client.so", RTLD_NOLOAD);
    if (!handle) {
        handle = dlopen("libcamera_client.so", RTLD_NOW);
    }
    if (!handle) {
        return false;
    }
    g_copy_ctor = reinterpret_cast<CopyCtorFn>(dlsym(handle, "_ZN7android14CameraMetadataC1ERKS0_"));
    g_dtor = reinterpret_cast<DtorFn>(dlsym(handle, "_ZN7android14CameraMetadataD1Ev"));
    g_find_const = reinterpret_cast<FindConstFn>(dlsym(handle, "_ZNK7android14CameraMetadata4findEj"));
    g_update_i32 = reinterpret_cast<UpdateInt32Fn>(dlsym(handle, "_ZN7android14CameraMetadata6updateEjPKim"));
    g_update_f = reinterpret_cast<UpdateFloatFn>(dlsym(handle, "_ZN7android14CameraMetadata6updateEjPKfm"));
    return g_copy_ctor && g_dtor && g_find_const && g_update_i32 && g_update_f;
}

static uint8_t* get_native_context(JNIEnv* env, jobject thiz) {
    if (!g_native_ctx_fid) {
        jclass cls = env->GetObjectClass(thiz);
        if (!cls) return nullptr;
        g_native_ctx_fid = env->GetFieldID(cls, "mNativeContext", "J");
        env->DeleteLocalRef(cls);
        if (!g_native_ctx_fid) return nullptr;
    }
    jlong ptr = env->GetLongField(thiz, g_native_ctx_fid);
    return reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(ptr));
}

struct ScopedDngMetadataOverride {
    RawCameraMetadata* chars = nullptr;
    RawCameraMetadata* result = nullptr;
    RawCameraMetadata saved_chars{};
    RawCameraMetadata saved_result{};
    RawCameraMetadata mod_chars{};
    RawCameraMetadata mod_result{};
    bool active_chars = false;
    bool active_result = false;

    ScopedDngMetadataOverride(JNIEnv* env, jobject thiz, jint width, jint height) {
        if (width <= 0 || height <= 0 || !resolve_camera_metadata_syms()) {
            return;
        }
        uint8_t* ctx = get_native_context(env, thiz);
        if (!ctx) {
            return;
        }
        chars = *reinterpret_cast<RawCameraMetadata**>(ctx + OFF_CTX_CHARACTERISTICS);
        result = *reinterpret_cast<RawCameraMetadata**>(ctx + OFF_CTX_RESULT);
        if (!chars) {
            return;
        }

        CameraMetadataRoEntry pix = g_find_const(chars, TAG_SENSOR_INFO_PIXEL_ARRAY_SIZE);
        CameraMetadataRoEntry pre = g_find_const(chars, TAG_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE);
        bool full_res_fallback = false;
        if (pix.count >= 2 && pix.data.i32 && pre.count >= 4 && pre.data.i32) {
            uint32_t w = static_cast<uint32_t>(width);
            uint32_t h = static_cast<uint32_t>(height);
            uint32_t pw = static_cast<uint32_t>(pix.data.i32[0]);
            uint32_t ph = static_cast<uint32_t>(pix.data.i32[1]);
            uint32_t cw = static_cast<uint32_t>(pre.data.i32[2]);
            uint32_t ch = static_cast<uint32_t>(pre.data.i32[3]);
            full_res_fallback = (w == pw * 2 && h == ph * 2 && w == cw * 2 && h == ch * 2);
        }

        uint8_t has_desc = *(ctx + OFF_CTX_HAS_DESCRIPTION);
        const char* desc_str = *reinterpret_cast<const char* const*>(ctx + OFF_CTX_DESCRIPTION);
        bool ultra_raw = (has_desc != 0 && desc_str != nullptr && strcmp(desc_str, "Xiaomi Ultra RAW") == 0);

        if (!full_res_fallback && !ultra_raw) {
            return;
        }

        saved_chars = *chars;
        g_copy_ctor(&mod_chars, chars);
        active_chars = true;

        if (full_res_fallback) {
            int32_t pix_vals[2] = {width, height};
            g_update_i32(&mod_chars, TAG_SENSOR_INFO_PIXEL_ARRAY_SIZE, pix_vals, 2);

            int32_t pre_vals[4] = {pre.data.i32[0] * 2, pre.data.i32[1] * 2, width, height};
            g_update_i32(&mod_chars, TAG_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE, pre_vals, 4);

            CameraMetadataRoEntry act = g_find_const(&mod_chars, TAG_SENSOR_INFO_ACTIVE_ARRAY_SIZE);
            if (act.count >= 4 && act.data.i32) {
                int32_t act_vals[4] = {act.data.i32[0] * 2, act.data.i32[1] * 2, width, height};
                g_update_i32(&mod_chars, TAG_SENSOR_INFO_ACTIVE_ARRAY_SIZE, act_vals, 4);
            }
            ALOGI("Applied dada full-res RAW metadata fallback for %dx%d", width, height);
        }

        if (ultra_raw && result) {
            int32_t white = 16383;
            int32_t black_i[4] = {0, 0, 0, 0};
            float black_f[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            g_update_i32(&mod_chars, TAG_SENSOR_INFO_WHITE_LEVEL, &white, 1);
            g_update_i32(&mod_chars, TAG_SENSOR_BLACK_LEVEL_PATTERN, black_i, 4);

            saved_result = *result;
            g_copy_ctor(&mod_result, result);
            active_result = true;
            g_update_f(&mod_result, TAG_SENSOR_DYNAMIC_BLACK_LEVEL, black_f, 4);
            ALOGI("Applied dada Ultra RAW blackLevel=0 / whiteLevel=16383");
        }

        *chars = mod_chars;
        if (active_result) {
            *result = mod_result;
        }
    }

    ~ScopedDngMetadataOverride() {
        if (active_chars) {
            *chars = saved_chars;
            g_dtor(&mod_chars);
        }
        if (active_result) {
            *result = saved_result;
            g_dtor(&mod_result);
        }
    }
};

static void hook_nativeWriteImage(JNIEnv* env, jobject thiz, jobject outStream,
                                  jint width, jint height, jobject rawBuffer,
                                  jint rowStride, jint pixStride, jlong offset,
                                  jboolean isDirect) {
    ScopedDngMetadataOverride guard(env, thiz, width, height);
    g_orig_write_image(env, thiz, outStream, width, height, rawBuffer,
                       rowStride, pixStride, offset, isDirect);
}

static void hook_nativeWriteInputStream(JNIEnv* env, jobject thiz, jobject outStream,
                                        jobject inStream, jint width, jint height,
                                        jlong offset) {
    ScopedDngMetadataOverride guard(env, thiz, width, height);
    g_orig_write_stream(env, thiz, outStream, inStream, width, height, offset);
}

static bool patch_ptr_slot(uintptr_t addr, void* expected, void* replacement) {
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0) pg = 4096;
    uintptr_t page_start = addr & ~static_cast<uintptr_t>(pg - 1);
    if (mprotect(reinterpret_cast<void*>(page_start), static_cast<size_t>(pg), PROT_READ | PROT_WRITE) != 0) {
        ALOGE("mprotect RW failed at 0x%lx", static_cast<unsigned long>(addr));
        return false;
    }
    void** slot = reinterpret_cast<void**>(addr);
    bool matched = (*slot == expected);
    if (matched) {
        *slot = replacement;
        __sync_synchronize();
    } else {
        ALOGE("Unexpected slot value at 0x%lx: got %p expected %p",
              static_cast<unsigned long>(addr), *slot, expected);
    }
    mprotect(reinterpret_cast<void*>(page_start), static_cast<size_t>(pg), PROT_READ);
    return matched;
}

static int phdr_cb(struct dl_phdr_info* info, size_t, void* data) {
    if (!info->dlpi_name || !strstr(info->dlpi_name, "libandroid_runtime.so")) {
        return 0;
    }
    *reinterpret_cast<uintptr_t*>(data) = static_cast<uintptr_t>(info->dlpi_addr);
    return 1;
}

__attribute__((constructor)) static void init_dada_rtfix() {
    if (!is_dada_device()) {
        return;
    }
    uintptr_t base = 0;
    dl_iterate_phdr(phdr_cb, &base);
    if (base == 0) {
        ALOGE("libandroid_runtime.so not found via dl_iterate_phdr");
        return;
    }

    g_orig_write_image = reinterpret_cast<WriteImageFn>(base + OFF_NATIVE_WRITE_IMAGE);
    g_orig_write_stream = reinterpret_cast<WriteInputStreamFn>(base + OFF_NATIVE_WRITE_INPUT_STREAM);

    bool ok1 = patch_ptr_slot(base + OFF_JNI_WRITE_IMAGE_PTR,
                              reinterpret_cast<void*>(g_orig_write_image),
                              reinterpret_cast<void*>(&hook_nativeWriteImage));
    bool ok2 = patch_ptr_slot(base + OFF_JNI_WRITE_STREAM_PTR,
                              reinterpret_cast<void*>(g_orig_write_stream),
                              reinterpret_cast<void*>(&hook_nativeWriteInputStream));
    ALOGI("Initialized for dada (base=0x%lx, writeImg=%d, writeStream=%d)",
          static_cast<unsigned long>(base), ok1 ? 1 : 0, ok2 ? 1 : 0);
}

} // namespace
