#include <android/log.h>
#include <dlfcn.h>
#include <link.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/system_properties.h>
#include <unistd.h>

#define LOG_TAG "DadaCamFix"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

constexpr uint32_t TAG_XIAOMI_SCALER_AVAILABLE_STREAM_CONFIGURATIONS = 0x81b90000u;
constexpr int32_t FMT_RAW_SENSOR = 32; // 0x20
constexpr int32_t FMT_BLOB = 33;       // 0x21

struct Size {
    int32_t width;
    int32_t height;
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

using FindFn = CameraMetadataRoEntry (*)(const void* camera_metadata, uint32_t tag);
using GetMaxJpegFn = Size (*)(const void* info, bool isUltraHighResolution);
using RoundBufDimFn = bool (*)(int32_t width, int32_t height, int32_t format,
                               int32_t dataSpace, const void* info,
                               bool maxResolutionMode, int32_t* outWidth,
                               int32_t* outHeight, bool isPriv);

static FindFn g_find_fn = nullptr;
static GetMaxJpegFn g_orig_get_max_jpeg = nullptr;
static RoundBufDimFn g_orig_round_buf_dim = nullptr;

// Offsets from ColorOS 17 V6 /system/lib64/libcameraservice.so
constexpr uintptr_t OFF_GET_MAX_JPEG_IMPL = 0x3d4374;
constexpr uintptr_t OFF_ROUND_BUF_DIM_IMPL = 0x3d4700;
constexpr uintptr_t OFF_GOT_FIND_CONST = 0x522740;
constexpr uintptr_t OFF_GOT_GET_MAX_JPEG = 0x523890;
constexpr uintptr_t OFF_GOT_ROUND_BUF_DIM = 0x525240;

static bool is_dada_device() {
    char prop[PROP_VALUE_MAX] = {0};
    __system_property_get("ro.product.device", prop);
    return strcmp(prop, "dada") == 0;
}

static Size hook_getMaxJpegResolution(const void* info, bool isUltraHighResolution) {
    Size orig = g_orig_get_max_jpeg ? g_orig_get_max_jpeg(info, isUltraHighResolution) : Size{0, 0};
    if (!isUltraHighResolution || !info || !g_find_fn) {
        return orig;
    }

    CameraMetadataRoEntry entry = g_find_fn(info, TAG_XIAOMI_SCALER_AVAILABLE_STREAM_CONFIGURATIONS);
    if (entry.count < 4 || (entry.count % 4) != 0 || !entry.data.i32) {
        return orig;
    }

    Size std_res = g_orig_get_max_jpeg(info, false);
    int64_t best_area = static_cast<int64_t>(orig.width) * static_cast<int64_t>(orig.height);
    Size best = orig;
    for (size_t i = 0; i + 3 < entry.count; i += 4) {
        int32_t fmt = entry.data.i32[i];
        int32_t w = entry.data.i32[i + 1];
        int32_t h = entry.data.i32[i + 2];
        int32_t is_input = entry.data.i32[i + 3];
        if (is_input == 0 && fmt == FMT_BLOB && w > 0 && h > 0) {
            // Prefer exact 2x standard resolution (8192x6144) so UHR JPEG buffer matches HAL CameraBlob trailer.
            if (std_res.width > 0 && std_res.height > 0 &&
                w == std_res.width * 2 && h == std_res.height * 2) {
                return Size{w, h};
            }
            int64_t area = static_cast<int64_t>(w) * static_cast<int64_t>(h);
            if (area > best_area) {
                best_area = area;
                best = Size{w, h};
            }
        }
    }
    return best;
}

static bool hook_roundBufferDimensionNearest(int32_t width, int32_t height, int32_t format,
                                             int32_t dataSpace, const void* info,
                                             bool maxResolutionMode, int32_t* outWidth,
                                             int32_t* outHeight, bool isPriv) {
    if ((format == FMT_RAW_SENSOR || format == FMT_BLOB) && info && g_find_fn && outWidth && outHeight) {
        CameraMetadataRoEntry entry = g_find_fn(info, TAG_XIAOMI_SCALER_AVAILABLE_STREAM_CONFIGURATIONS);
        if (entry.count >= 4 && (entry.count % 4) == 0 && entry.data.i32) {
            for (size_t i = 0; i + 3 < entry.count; i += 4) {
                int32_t fmt = entry.data.i32[i];
                int32_t w = entry.data.i32[i + 1];
                int32_t h = entry.data.i32[i + 2];
                int32_t is_input = entry.data.i32[i + 3];
                if (is_input == 0 && fmt == format && w == width && h == height) {
                    *outWidth = width;
                    *outHeight = height;
                    return true;
                }
            }
        }
    }
    if (g_orig_round_buf_dim) {
        return g_orig_round_buf_dim(width, height, format, dataSpace, info,
                                    maxResolutionMode, outWidth, outHeight, isPriv);
    }
    return false;
}

static bool patch_got_slot(uintptr_t addr, void* new_val) {
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0) pg = 4096;
    uintptr_t page_start = addr & ~static_cast<uintptr_t>(pg - 1);
    if (mprotect(reinterpret_cast<void*>(page_start), static_cast<size_t>(pg), PROT_READ | PROT_WRITE) != 0) {
        ALOGE("mprotect RW failed at 0x%lx", static_cast<unsigned long>(addr));
        return false;
    }
    *reinterpret_cast<void**>(addr) = new_val;
    __sync_synchronize();
    mprotect(reinterpret_cast<void*>(page_start), static_cast<size_t>(pg), PROT_READ);
    return true;
}

static int phdr_cb(struct dl_phdr_info* info, size_t, void* data) {
    if (!info->dlpi_name || !strstr(info->dlpi_name, "libcameraservice.so")) {
        return 0;
    }
    *reinterpret_cast<uintptr_t*>(data) = static_cast<uintptr_t>(info->dlpi_addr);
    return 1;
}

__attribute__((constructor)) static void init_dada_camfix() {
    if (!is_dada_device()) {
        return;
    }
    uintptr_t base = 0;
    dl_iterate_phdr(phdr_cb, &base);
    if (base == 0) {
        ALOGE("libcameraservice.so not found via dl_iterate_phdr");
        return;
    }

    g_orig_get_max_jpeg = reinterpret_cast<GetMaxJpegFn>(base + OFF_GET_MAX_JPEG_IMPL);
    g_orig_round_buf_dim = reinterpret_cast<RoundBufDimFn>(base + OFF_ROUND_BUF_DIM_IMPL);
    g_find_fn = *reinterpret_cast<FindFn*>(base + OFF_GOT_FIND_CONST);
    if (!g_find_fn) {
        g_find_fn = reinterpret_cast<FindFn>(dlsym(RTLD_DEFAULT, "_ZNK7android14CameraMetadata4findEj"));
    }

    bool ok1 = patch_got_slot(base + OFF_GOT_GET_MAX_JPEG, reinterpret_cast<void*>(&hook_getMaxJpegResolution));
    bool ok2 = patch_got_slot(base + OFF_GOT_ROUND_BUF_DIM, reinterpret_cast<void*>(&hook_roundBufferDimensionNearest));
    ALOGI("Initialized for dada (base=0x%lx, getMaxJpeg=%d, roundBufDim=%d)",
          static_cast<unsigned long>(base), ok1 ? 1 : 0, ok2 ? 1 : 0);
}

} // namespace
