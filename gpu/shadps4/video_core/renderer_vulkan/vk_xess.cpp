// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "video_core/renderer_vulkan/vk_xess.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "bbport_settings.h"

namespace Vulkan {

namespace {

// Minimal types matching Intel XeSS C API (xess.h & xess_vk.h)
using xess_context_handle_t = void*;

enum xess_result_t : int {
    XESS_RESULT_SUCCESS = 0,
    XESS_RESULT_ERROR_UNSUPPORTED_DEVICE = 1,
    XESS_RESULT_ERROR_UNSUPPORTED_DRIVER = 2,
    XESS_RESULT_ERROR_UNINITIALIZED = 3,
    XESS_RESULT_ERROR_INVALID_ARGUMENT = 4,
    XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY = 5,
    XESS_RESULT_ERROR_DEVICE_LOST = 6,
    XESS_RESULT_ERROR_UNKNOWN = 1000,
};

#pragma pack(push, 8)
struct xess_version_t {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
    uint16_t reserved;
};

struct xess_2d_t {
    uint32_t x;
    uint32_t y;
};

enum xess_quality_settings_t : int {
    XESS_QUALITY_SETTING_ULTRA_PERFORMANCE = 100,
    XESS_QUALITY_SETTING_PERFORMANCE = 101,
    XESS_QUALITY_SETTING_BALANCED = 102,
    XESS_QUALITY_SETTING_QUALITY = 103,
    XESS_QUALITY_SETTING_ULTRA_QUALITY = 104,
    XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS = 105,
    XESS_QUALITY_SETTING_AA = 106,
};

enum xess_init_flags_t : uint32_t {
    XESS_INIT_FLAG_NONE = 0,
    XESS_INIT_FLAG_HIGH_RES_MV = 1 << 0,
    XESS_INIT_FLAG_INVERTED_DEPTH = 1 << 1,
    XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE = 1 << 2,
    XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK = 1 << 3,
    XESS_INIT_FLAG_USE_NDC_VELOCITY = 1 << 4,
    XESS_INIT_FLAG_EXTERNAL_DESCRIPTOR_HEAP = 1 << 5,
    XESS_INIT_FLAG_LDR_INPUT_COLOR = 1 << 6,
    XESS_INIT_FLAG_JITTERED_MV = 1 << 7,
    XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE = 1 << 8,
};

struct xess_vk_init_params_t {
    xess_2d_t outputResolution;
    xess_quality_settings_t qualitySetting;
    uint32_t initFlags;
    uint32_t creationNodeMask;
    uint32_t visibleNodeMask;
    VkDeviceMemory tempBufferHeap;
    uint64_t bufferHeapOffset;
    VkDeviceMemory tempTextureHeap;
    uint64_t textureHeapOffset;
    VkPipelineCache pipelineCache;
};

struct xess_vk_image_view_info {
    VkImageView imageView;
    VkImage image;
    VkImageSubresourceRange subresourceRange;
    VkFormat format;
    unsigned int width;
    unsigned int height;
};

struct xess_vk_execute_params_t {
    xess_vk_image_view_info colorTexture;
    xess_vk_image_view_info velocityTexture;
    xess_vk_image_view_info depthTexture;
    xess_vk_image_view_info exposureScaleTexture;
    xess_vk_image_view_info responsivePixelMaskTexture;
    xess_vk_image_view_info outputTexture;

    float jitterOffsetX;
    float jitterOffsetY;
    float exposureScale;
    uint32_t resetHistory;
    uint32_t inputWidth;
    uint32_t inputHeight;
    xess_2d_t inputColorBase;
    xess_2d_t inputMotionVectorBase;
    xess_2d_t inputDepthBase;
    xess_2d_t inputResponsiveMaskBase;
    xess_2d_t reserved0;
    xess_2d_t outputColorBase;
};
#pragma pack(pop)

using PFN_xessGetVersion = xess_result_t (*)(xess_version_t*);
using PFN_xessVKCreateContext = xess_result_t (*)(VkInstance, VkPhysicalDevice, VkDevice, xess_context_handle_t*);
using PFN_xessDestroyContext = xess_result_t (*)(xess_context_handle_t);
using PFN_xessVKInit = xess_result_t (*)(xess_context_handle_t, const xess_vk_init_params_t*);
using PFN_xessVKExecute = xess_result_t (*)(xess_context_handle_t, VkCommandBuffer, const xess_vk_execute_params_t*);
using PFN_xessSetJitterScale = xess_result_t (*)(xess_context_handle_t, float, float);
using PFN_xessSetVelocityScale = xess_result_t (*)(xess_context_handle_t, float, float);

xess_quality_settings_t Quality(int preset) {
    switch (preset) {
    case BbSettings::NativeAA:
        return XESS_QUALITY_SETTING_AA;
    case BbSettings::Quality:
        return XESS_QUALITY_SETTING_QUALITY;
    case BbSettings::Balanced:
        return XESS_QUALITY_SETTING_BALANCED;
    case BbSettings::Performance:
        return XESS_QUALITY_SETTING_PERFORMANCE;
    default:
        return XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
    }
}

bool IsFloatFormat(vk::Format f) {
    switch (f) {
    case vk::Format::eR16G16B16A16Sfloat:
    case vk::Format::eR32G32B32A32Sfloat:
    case vk::Format::eB10G11R11UfloatPack32:
    case vk::Format::eR16G16B16Sfloat:
        return true;
    default:
        return false;
    }
}

xess_vk_image_view_info MakeViewInfo(const Fsr4Upscaler::Image& img, VkImageAspectFlags aspect) {
    return xess_vk_image_view_info{
        .imageView = static_cast<VkImageView>(img.view),
        .image = static_cast<VkImage>(img.image),
        .subresourceRange = {aspect, 0, 1, 0, 1},
        .format = static_cast<VkFormat>(img.format),
        .width = img.width,
        .height = img.height,
    };
}

static void ImageBarrier(vk::CommandBuffer cmdbuf, vk::Image image, vk::ImageAspectFlags aspect,
                         vk::ImageLayout old_layout, vk::ImageLayout new_layout,
                         vk::AccessFlags2 src_access, vk::AccessFlags2 dst_access,
                         vk::PipelineStageFlags2 src_stage, vk::PipelineStageFlags2 dst_stage) {
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {aspect, 0, 1, 0, 1},
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
}

} // namespace

struct XessUpscaler::Impl {
    const Instance& instance;
    Scheduler& scheduler;
    bool available = false;
    bool fatal = false;
    std::string problem;

#ifdef _WIN32
    HMODULE lib = nullptr;
#else
    void* lib = nullptr;
#endif

    PFN_xessGetVersion get_version = nullptr;
    PFN_xessVKCreateContext create_context = nullptr;
    PFN_xessDestroyContext destroy_context = nullptr;
    PFN_xessVKInit init = nullptr;
    PFN_xessVKExecute execute = nullptr;
    PFN_xessSetJitterScale set_jitter_scale = nullptr;
    PFN_xessSetVelocityScale set_velocity_scale = nullptr;

    bool initialized = false;
    xess_context_handle_t context = nullptr;

    struct Key {
        u32 render_width, render_height, out_width, out_height;
        int quality;
        bool hdr;
        bool operator==(const Key&) const = default;
    } key{};

    Impl(const Instance& instance_, Scheduler& scheduler_)
        : instance{instance_}, scheduler{scheduler_} {}

    void Fail(std::string why, bool permanent) {
        if (problem != why) {
            std::printf("XeSS: %s\n", why.c_str());
        }
        problem = std::move(why);
        fatal = fatal || permanent;
    }

    template <typename Fn>
    bool Load(Fn& fn, const char* name) {
#ifdef _WIN32
        fn = reinterpret_cast<Fn>(GetProcAddress(lib, name));
#else
        fn = reinterpret_cast<Fn>(dlsym(lib, name));
#endif
        return fn != nullptr;
    }

    std::vector<std::filesystem::path> SearchPaths() {
        std::vector<std::filesystem::path> paths;
        if (const char* env = std::getenv("BB_XESS_DIR"); env && *env) {
            paths.emplace_back(env);
        }
#ifdef _WIN32
        wchar_t exe[MAX_PATH];
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
            paths.push_back(std::filesystem::path(exe).parent_path());
        }
#else
        std::error_code ec;
        const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (!ec) {
            paths.push_back(exe.parent_path());
        }
#endif
        paths.emplace_back("out");
        paths.emplace_back(".");
        return paths;
    }

    bool LoadLibraryFile() {
        const auto paths = SearchPaths();
#ifdef _WIN32
        const char* lib_name = "libxess.dll";
#else
        const char* lib_name = "libxess.so";
#endif
        for (const auto& p : paths) {
            auto candidate = p / lib_name;
#ifdef _WIN32
            lib = LoadLibraryW(candidate.wstring().c_str());
#else
            lib = dlopen(candidate.string().c_str(), RTLD_NOW);
#endif
            if (lib) {
                return true;
            }
        }
#ifdef _WIN32
        lib = LoadLibraryA(lib_name);
#else
        lib = dlopen(lib_name, RTLD_NOW);
#endif
        return lib != nullptr;
    }

    void Initialize() {
        if (!LoadLibraryFile()) {
            Fail("libxess not found (libxess.dll / libxess.so in out/, BB_XESS_DIR or system PATH; tools/fetch_xess.sh)",
                 true);
            return;
        }

        Load(get_version, "xessGetVersion");
        if (!Load(create_context, "xessVKCreateContext") ||
            !Load(destroy_context, "xessDestroyContext") ||
            !Load(init, "xessVKInit") ||
            !Load(execute, "xessVKExecute")) {
            Fail("the XeSS library lacks required Vulkan entry points", true);
            return;
        }

        Load(set_jitter_scale, "xessSetJitterScale");
        Load(set_velocity_scale, "xessSetVelocityScale");

        initialized = true;
        available = true;

        if (get_version) {
            xess_version_t v{};
            if (get_version(&v) == XESS_RESULT_SUCCESS) {
                std::printf("XeSS: libxess %u.%u.%u loaded successfully\n", v.major, v.minor, v.patch);
            }
        } else {
            std::printf("XeSS: libxess loaded successfully\n");
        }
    }

    bool Record(const Fsr4Upscaler::Frame& f) {
        if (!available) {
            return false;
        }
        const Key wanted{f.render_width, f.render_height, f.output.width, f.output.height,
                         Quality(f.preset), IsFloatFormat(f.color.format)};

        if (!context || !(wanted == key)) {
            if (context) {
                scheduler.Finish();
                destroy_context(context);
                context = nullptr;
            }
            const auto res_ctx = create_context(
                VkInstance(instance.GetInstance()),
                VkPhysicalDevice(instance.GetPhysicalDevice()),
                VkDevice(instance.GetDevice()),
                &context);
            if (res_ctx != XESS_RESULT_SUCCESS || !context) {
                context = nullptr;
                Fail("xessVKCreateContext failed (error " + std::to_string(res_ctx) + ")", false);
                return false;
            }

            uint32_t flags = XESS_INIT_FLAG_NONE;
            if (wanted.hdr) {
                flags |= XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE;
            } else {
                flags |= XESS_INIT_FLAG_LDR_INPUT_COLOR;
            }

            const xess_vk_init_params_t init_params{
                .outputResolution = {wanted.out_width, wanted.out_height},
                .qualitySetting = static_cast<xess_quality_settings_t>(wanted.quality),
                .initFlags = flags,
                .creationNodeMask = 1,
                .visibleNodeMask = 1,
                .tempBufferHeap = VK_NULL_HANDLE,
                .bufferHeapOffset = 0,
                .tempTextureHeap = VK_NULL_HANDLE,
                .textureHeapOffset = 0,
                .pipelineCache = VK_NULL_HANDLE,
            };

            const auto res_init = init(context, &init_params);
            if (res_init != XESS_RESULT_SUCCESS) {
                destroy_context(context);
                context = nullptr;
                Fail("xessVKInit failed (error " + std::to_string(res_init) + ")", false);
                return false;
            }
            key = wanted;
            std::printf("XeSS: %ux%u -> %ux%u, quality mode %d, %s input\n",
                        key.render_width, key.render_height, key.out_width, key.out_height,
                        key.quality, key.hdr ? "HDR" : "LDR");
        }

        // Transition inputs from GENERAL to SHADER_READ_ONLY_OPTIMAL
        ImageBarrier(f.cmdbuf, f.color.image, vk::ImageAspectFlagBits::eColor,
                     vk::ImageLayout::eGeneral, vk::ImageLayout::eShaderReadOnlyOptimal,
                     vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eColorAttachmentWrite,
                     vk::AccessFlagBits2::eShaderRead,
                     vk::PipelineStageFlagBits2::eAllCommands, vk::PipelineStageFlagBits2::eComputeShader);

        ImageBarrier(f.cmdbuf, f.depth.image, vk::ImageAspectFlagBits::eDepth,
                     vk::ImageLayout::eGeneral, vk::ImageLayout::eShaderReadOnlyOptimal,
                     vk::AccessFlagBits2::eDepthStencilAttachmentWrite, vk::AccessFlagBits2::eShaderRead,
                     vk::PipelineStageFlagBits2::eAllCommands, vk::PipelineStageFlagBits2::eComputeShader);

        ImageBarrier(f.cmdbuf, f.motion.image, vk::ImageAspectFlagBits::eColor,
                     vk::ImageLayout::eGeneral, vk::ImageLayout::eShaderReadOnlyOptimal,
                     vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eColorAttachmentWrite,
                     vk::AccessFlagBits2::eShaderRead,
                     vk::PipelineStageFlagBits2::eAllCommands, vk::PipelineStageFlagBits2::eComputeShader);

        const xess_vk_execute_params_t exec_params{
            .colorTexture = MakeViewInfo(f.color, VK_IMAGE_ASPECT_COLOR_BIT),
            .velocityTexture = MakeViewInfo(f.motion, VK_IMAGE_ASPECT_COLOR_BIT),
            .depthTexture = MakeViewInfo(f.depth, VK_IMAGE_ASPECT_DEPTH_BIT),
            .exposureScaleTexture = {},
            .responsivePixelMaskTexture = {},
            .outputTexture = MakeViewInfo(f.output, VK_IMAGE_ASPECT_COLOR_BIT),
            .jitterOffsetX = f.jitter[0],
            .jitterOffsetY = f.jitter[1],
            .exposureScale = 1.0f,
            .resetHistory = f.reset ? 1u : 0u,
            .inputWidth = f.render_width,
            .inputHeight = f.render_height,
            .inputColorBase = {0, 0},
            .inputMotionVectorBase = {0, 0},
            .inputDepthBase = {0, 0},
            .inputResponsiveMaskBase = {0, 0},
            .reserved0 = {0, 0},
            .outputColorBase = {0, 0},
        };

        const auto res_exec = execute(context, VkCommandBuffer(f.cmdbuf), &exec_params);

        // Transition inputs back to GENERAL
        ImageBarrier(f.cmdbuf, f.color.image, vk::ImageAspectFlagBits::eColor,
                     vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eGeneral,
                     vk::AccessFlagBits2::eShaderRead,
                     vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                     vk::PipelineStageFlagBits2::eComputeShader, vk::PipelineStageFlagBits2::eAllCommands);

        ImageBarrier(f.cmdbuf, f.depth.image, vk::ImageAspectFlagBits::eDepth,
                     vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eGeneral,
                     vk::AccessFlagBits2::eShaderRead,
                     vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
                     vk::PipelineStageFlagBits2::eComputeShader, vk::PipelineStageFlagBits2::eAllCommands);

        ImageBarrier(f.cmdbuf, f.motion.image, vk::ImageAspectFlagBits::eColor,
                     vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eGeneral,
                     vk::AccessFlagBits2::eShaderRead,
                     vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                     vk::PipelineStageFlagBits2::eComputeShader, vk::PipelineStageFlagBits2::eAllCommands);

        if (res_exec != XESS_RESULT_SUCCESS) {
            Fail("xessVKExecute failed (error " + std::to_string(res_exec) + ")", false);
            return false;
        }
        problem.clear();
        return true;
    }

    ~Impl() {
        if (!initialized) {
            return;
        }
        scheduler.Finish();
        if (context) {
            destroy_context(context);
            context = nullptr;
        }
#ifdef _WIN32
        if (lib) {
            FreeLibrary(lib);
            lib = nullptr;
        }
#else
        if (lib) {
            dlclose(lib);
            lib = nullptr;
        }
#endif
    }
};

XessUpscaler::XessUpscaler(const Instance& instance, Scheduler& scheduler)
    : impl{std::make_unique<Impl>(instance, scheduler)} {
    impl->Initialize();
}

XessUpscaler::~XessUpscaler() = default;

bool XessUpscaler::Available() const noexcept {
    return impl && impl->available;
}

bool XessUpscaler::Record(const Fsr4Upscaler::Frame& frame) {
    return impl && impl->Record(frame);
}

const char* XessUpscaler::Problem() const noexcept {
    return impl && !impl->problem.empty() ? impl->problem.c_str() : nullptr;
}

bool XessUpscaler::Fatal() const noexcept {
    return impl && impl->fatal;
}

} // namespace Vulkan
