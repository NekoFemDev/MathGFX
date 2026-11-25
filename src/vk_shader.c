/*
 * mathgfx-vk — headless Vulkan compute black-hole renderer.
 *
 * No window, no surface: a compute shader writes packed RGB words into a
 * storage buffer, the host reads it back and writes PNG. The shader is
 * the GPU port of examples/blackhole.mgf (Schwarzschild geodesics +
 * thin accretion disk).
 */
#include <vulkan/vulkan.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mgfx.h"
#include "video.h"
#include "portable.h"

#define VKCHK(x)                                                              \
    do {                                                                      \
        VkResult r_ = (x);                                                    \
        if (r_ != VK_SUCCESS) {                                               \
            fprintf(stderr, "mathgfx-vk: Vulkan error %d at %s:%d\n",         \
                    (int)r_, __FILE__, __LINE__);                             \
            exit(1);                                                          \
        }                                                                     \
    } while (0)

static void usage(FILE *f)
{
    fprintf(f,
        "mathgfx-vk — Vulkan compute black hole (headless)\n"
        "\n"
        "usage:\n"
        "  mathgfx-vk [--shader build/blackhole.spv] [--width N] [--height N]\n"
        "             [--ssaa N] [--gpu N] [--time SECONDS] [--time-end SECONDS]\n"
        "             [--dof N] [--aperture A] [--focus F]\n"
        "             [--loop-period SECONDS] [--rigid-rotation]\n"
        "             [--out out/blackhole_vk.png] [--video out.avi]\n"
        "             [--frames N] [--fps N] [--quality N] [--list-gpus]\n"
        "\n"
        "  --shader P         SPIR-V compute shader (default build/blackhole.spv)\n"
        "  --ssaa N           render at N*N supersampling then box-downsample (1..4)\n"
        "  --gpu N            physical device index (see --list-gpus; default: first\n"
        "                     device with a compute queue)\n"
        "  --time T           start time uniform for the shader (default 0)\n"
        "  --time-end T       end time for video (default: time + frames/fps).\n"
        "                     If set, time interpolates linearly from --time to\n"
        "                     --time-end over all frames. Use for seamless loops.\n"
        "  --loop-period T    if > 0, time wraps modulo T in shader (seamless loops\n"
        "                     with blackhole_loop.spv). Use with --time-end.\n"
        "  --rigid-rotation   uniform disk rotation (same omega at all radii)\n"
        "                     instead of Keplerian differential rotation.\n"
        "                     Requires blackhole_loop.spv.\n"
        "  --dof N            thin-lens samples per pixel (default 1)\n"
        "  --aperture A       lens radius in rs (default 0 = pinhole)\n"
        "  --focus F          focus distance in rs (default 24)\n"
        "  --focus-end F      animate focus from F to this value and back\n"
        "  --video P          render N frames into an MJPEG AVI instead of a PNG\n"
        "  --frames N         number of video frames (required with --video)\n"
        "  --fps N            video frame rate (default 25)\n"
        "  --quality N        JPEG quality 1..100 (default 90)\n"
        "  --crf N            H.264 CRF 0..51 for .mp4 (default 20)\n");
}

static int parse_int(const char *s, int *out)
{
    char *end = NULL;
    long v;
    if (!s || !*s)
        return -1;
    v = strtol(s, &end, 10);
    if (!end || *end || v < 0 || v > 1000000)
        return -1;
    *out = (int)v;
    return 0;
}

static const char *device_type_name(VkPhysicalDeviceType t)
{
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "cpu";
    default:                                     return "other";
    }
}

static int has_compute(VkPhysicalDevice dev, uint32_t *family_out)
{
    uint32_t n = 0;
    uint32_t i;
    VkQueueFamilyProperties *q;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &n, NULL);
    if (n == 0)
        return 0;
    q = (VkQueueFamilyProperties *)malloc((size_t)n * sizeof(*q));
    if (!q)
        return 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &n, q);
    for (i = 0; i < n; i++) {
        if ((q[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && q[i].queueCount > 0) {
            *family_out = i;
            free(q);
            return 1;
        }
    }
    free(q);
    return 0;
}

static uint32_t find_memory_type(VkPhysicalDevice dev, uint32_t type_bits,
                                 VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t i;
    vkGetPhysicalDeviceMemoryProperties(dev, &mp);
    for (i = 0; i < mp.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    }
    return UINT32_MAX;
}

typedef struct {
    uint32_t w;
    uint32_t h;
    float time;
    uint32_t frame;
    float aperture;
    float focus;
    uint32_t samples;
    uint32_t seed;
    float loop_period;    // if > 0, time wraps modulo this
    uint32_t rigid_rotation; // if 1, uniform rotation instead of Keplerian
    float duration;       // video duration (time_end - time_start) for Keplerian seamless mode
    uint32_t total_frames; // total frames in video (for seamless noise wrapping)
} PushParams;

static void record_dispatch(VkCommandBuffer cmd, VkPipeline pipeline,
                            VkPipelineLayout layout, VkDescriptorSet set,
                            VkBuffer buffer, const PushParams *pc)
{
    VkCommandBufferBeginInfo bi;
    VkBufferMemoryBarrier bmb;
    int rw = (int)pc->w;
    int rh = (int)pc->h;

    VKCHK(vkResetCommandBuffer(cmd, 0));
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKCHK(vkBeginCommandBuffer(cmd, &bi));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(*pc), pc);
    vkCmdDispatch(cmd, (uint32_t)((rw + 7) / 8),
                  (uint32_t)((rh + 7) / 8), 1);

    memset(&bmb, 0, sizeof(bmb));
    bmb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bmb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bmb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bmb.buffer = buffer;
    bmb.offset = 0;
    bmb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &bmb,
                         0, NULL);
    VKCHK(vkEndCommandBuffer(cmd));
}

static void downsample_rgb(const uint32_t *src, int rw, int width, int height,
                           int ssaa, unsigned char *rgb)
{
    int x, y;
    if (ssaa == 1) {
        size_t npx = (size_t)width * (size_t)height;
        size_t i;
        for (i = 0; i < npx; i++) {
            uint32_t p = src[i];
            rgb[i * 3 + 0] = (unsigned char)(p & 255u);
            rgb[i * 3 + 1] = (unsigned char)((p >> 8) & 255u);
            rgb[i * 3 + 2] = (unsigned char)((p >> 16) & 255u);
        }
        return;
    }
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            uint64_t r = 0, g = 0, b = 0;
            int sy, sx;
            uint32_t n = (uint32_t)(ssaa * ssaa);
            size_t o = ((size_t)y * width + x) * 3u;
            for (sy = 0; sy < ssaa; sy++) {
                for (sx = 0; sx < ssaa; sx++) {
                    uint32_t p = src[((size_t)(y * ssaa + sy) * rw) +
                                    (x * ssaa + sx)];
                    r += p & 255u;
                    g += (p >> 8) & 255u;
                    b += (p >> 16) & 255u;
                }
            }
            rgb[o + 0] = (unsigned char)((r + n / 2) / n);
            rgb[o + 1] = (unsigned char)((g + n / 2) / n);
            rgb[o + 2] = (unsigned char)((b + n / 2) / n);
        }
    }
}

int main(int argc, char **argv)
{
    int width = 1920;
    int height = 1080;
    int ssaa = 1;
    int gpu_index = -1;
    int list_gpus = 0;
    float time_val = 0.0f;
    float time_end = -1.0f;
    float aperture = 0.0f;
    float focus = 24.0f;
    float focus_end = -1.0f;
    int dof_samples = 1;
    float loop_period = -1.0f;
    int rigid_rotation = 0;
    float duration = -1.0f;
    const char *shader_path = "build/blackhole.spv";
    const char *out_path = "out/blackhole_vk.png";
    const char *video_path = NULL;
    int frames = 0;
    int fps = 25;
    int quality = 90;
    int crf = 20;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &width) != 0 || width < 1 || width > 16384)
                goto bad_args;
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &height) != 0 || height < 1 || height > 16384)
                goto bad_args;
        } else if (strcmp(argv[i], "--ssaa") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &ssaa) != 0 || ssaa < 1 || ssaa > 4)
                goto bad_args;
        } else if (strcmp(argv[i], "--gpu") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &gpu_index) != 0)
                goto bad_args;
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(argv[i], "--shader") == 0 && i + 1 < argc) {
            shader_path = argv[++i];
        } else if (strcmp(argv[i], "--video") == 0 && i + 1 < argc) {
            video_path = argv[++i];
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &frames) != 0 || frames < 1 || frames > 100000)
                goto bad_args;
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &fps) != 0 || fps < 1 || fps > 240)
                goto bad_args;
        } else if (strcmp(argv[i], "--quality") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &quality) != 0 || quality < 1 || quality > 100)
                goto bad_args;
        } else if (strcmp(argv[i], "--crf") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &crf) != 0 || crf < 0 || crf > 51)
                goto bad_args;
        } else if (strcmp(argv[i], "--time") == 0 && i + 1 < argc) {
            char *end = NULL;
            time_val = strtof(argv[++i], &end);
            if (!end || *end || time_val < 0.0f || time_val > 1.0e6f)
                goto bad_args;
        } else if (strcmp(argv[i], "--time-end") == 0 && i + 1 < argc) {
            char *end = NULL;
            time_end = strtof(argv[++i], &end);
            if (!end || *end || time_end < 0.0f || time_end > 1.0e6f)
                goto bad_args;
        } else if (strcmp(argv[i], "--dof") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &dof_samples) != 0 ||
                dof_samples < 1 || dof_samples > 512)
                goto bad_args;
        } else if (strcmp(argv[i], "--aperture") == 0 && i + 1 < argc) {
            char *end = NULL;
            aperture = strtof(argv[++i], &end);
            if (!end || *end || aperture < 0.0f || aperture > 10.0f)
                goto bad_args;
        } else if (strcmp(argv[i], "--focus") == 0 && i + 1 < argc) {
            char *end = NULL;
            focus = strtof(argv[++i], &end);
            if (!end || *end || focus < 0.1f || focus > 1000.0f)
                goto bad_args;
        } else if (strcmp(argv[i], "--focus-end") == 0 && i + 1 < argc) {
            char *end = NULL;
            focus_end = strtof(argv[++i], &end);
            if (!end || *end || focus_end < 0.1f || focus_end > 1000.0f)
                goto bad_args;
        } else if (strcmp(argv[i], "--loop-period") == 0 && i + 1 < argc) {
            char *end = NULL;
            loop_period = strtof(argv[++i], &end);
            if (!end || *end || loop_period < 0.0f || loop_period > 1.0e6f)
                goto bad_args;
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            char *end = NULL;
            duration = strtof(argv[++i], &end);
            if (!end || *end || duration < 0.0f || duration > 1.0e6f)
                goto bad_args;
        } else if (strcmp(argv[i], "--rigid-rotation") == 0) {
            rigid_rotation = 1;
        } else if (strcmp(argv[i], "--list-gpus") == 0) {
            list_gpus = 1;
        } else {
            goto bad_args;
        }
    }

    if (width <= 0 || height <= 0 || ssaa <= 0) {
bad_args:
        usage(stderr);
        return 2;
    }

    {
        int rw = width * ssaa;
        int rh = height * ssaa;
        VkInstance instance;
        VkApplicationInfo app;
        VkInstanceCreateInfo ici;
        uint32_t ndev = 0;
        VkPhysicalDevice *devs;
        VkPhysicalDevice dev = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties props;
        uint32_t qfamily = 0;
        VkDevice device;
        VkQueue queue;
        VkDeviceQueueCreateInfo qci;
        VkDeviceCreateInfo dci;
        float prio = 1.0f;
        VkBuffer buffer;
        VkMemoryRequirements memreq;
        VkDeviceMemory memory;
        void *mapped = NULL;
        VkDescriptorSetLayout set_layout;
        VkDescriptorSetLayoutBinding binding;
        VkDescriptorSetLayoutCreateInfo slci;
        VkDescriptorPool pool;
        VkDescriptorPoolSize psize;
        VkDescriptorPoolCreateInfo dpci;
        VkDescriptorSet set;
        VkDescriptorSetAllocateInfo dsai;
        VkDescriptorBufferInfo dbi;
        VkWriteDescriptorSet write;
        VkPushConstantRange pcr;
        VkPipelineLayout pipeline_layout;
        VkPipelineLayoutCreateInfo plci;
        VkShaderModule shader;
        VkShaderModuleCreateInfo smci;
        unsigned char *spv = NULL;
        size_t spv_size = 0;
        VkComputePipelineCreateInfo cpci;
        VkPipeline pipeline;
        VkCommandPool cmd_pool;
        VkCommandPoolCreateInfo cpci2;
        VkCommandBuffer cmd;
        VkCommandBufferAllocateInfo cbai;
        VkFence fence;
        VkFenceCreateInfo fci;
        VkSubmitInfo submit;
        VkDeviceSize bytes;
        uint32_t *src;
        uint32_t *snap;
        unsigned char *rgb;
        double t0, t1;
        char gpu_name[256];
        char err[600];

        memset(&app, 0, sizeof(app));
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "mathgfx-vk";
        app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        app.pEngineName = "MathGFX";
        app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        app.apiVersion = VK_API_VERSION_1_1;

        memset(&ici, 0, sizeof(ici));
        ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo = &app;
        VKCHK(vkCreateInstance(&ici, NULL, &instance));

        VKCHK(vkEnumeratePhysicalDevices(instance, &ndev, NULL));
        if (ndev == 0) {
            fprintf(stderr, "mathgfx-vk: no Vulkan physical devices\n");
            return 1;
        }
        devs = (VkPhysicalDevice *)calloc(ndev, sizeof(*devs));
        if (!devs) {
            fprintf(stderr, "mathgfx-vk: out of memory\n");
            return 1;
        }
        VKCHK(vkEnumeratePhysicalDevices(instance, &ndev, devs));

        if (list_gpus) {
            for (uint32_t k = 0; k < ndev; k++) {
                VkPhysicalDeviceProperties p;
                uint32_t qf;
                vkGetPhysicalDeviceProperties(devs[k], &p);
                printf("GPU %u: %s (%s, api %u.%u.%u, compute=%s)\n",
                       k, p.deviceName, device_type_name(p.deviceType),
                       VK_VERSION_MAJOR(p.apiVersion),
                       VK_VERSION_MINOR(p.apiVersion),
                       VK_VERSION_PATCH(p.apiVersion),
                       has_compute(devs[k], &qf) ? "yes" : "no");
            }
            free(devs);
            vkDestroyInstance(instance, NULL);
            return 0;
        }

        if (gpu_index >= 0) {
            if ((uint32_t)gpu_index >= ndev) {
                fprintf(stderr, "mathgfx-vk: --gpu %d out of range (0..%u)\n",
                        gpu_index, ndev - 1);
                return 1;
            }
            dev = devs[gpu_index];
            if (!has_compute(dev, &qfamily)) {
                fprintf(stderr, "mathgfx-vk: GPU %d has no compute queue\n",
                        gpu_index);
                return 1;
            }
        } else {
            for (uint32_t k = 0; k < ndev; k++) {
                if (has_compute(devs[k], &qfamily)) {
                    dev = devs[k];
                    gpu_index = (int)k;
                    break;
                }
            }
            if (dev == VK_NULL_HANDLE) {
                fprintf(stderr, "mathgfx-vk: no compute-capable GPU\n");
                return 1;
            }
        }

        vkGetPhysicalDeviceProperties(dev, &props);
        snprintf(gpu_name, sizeof(gpu_name), "%s", props.deviceName);

        memset(&qci, 0, sizeof(qci));
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = qfamily;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        memset(&dci, 0, sizeof(dci));
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        VKCHK(vkCreateDevice(dev, &dci, NULL, &device));
        vkGetDeviceQueue(device, qfamily, 0, &queue);

        bytes = (VkDeviceSize)rw * (VkDeviceSize)rh * 4u;
        if (bytes > props.limits.maxStorageBufferRange) {
            fprintf(stderr, "mathgfx-vk: %dx%d needs %llu bytes, "
                    "maxStorageBufferRange is %u\n",
                    rw, rh, (unsigned long long)bytes,
                    props.limits.maxStorageBufferRange);
            return 1;
        }

        {
            VkBufferCreateInfo bci;
            memset(&bci, 0, sizeof(bci));
            bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bci.size = bytes;
            bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VKCHK(vkCreateBuffer(device, &bci, NULL, &buffer));
        }
        vkGetBufferMemoryRequirements(device, buffer, &memreq);
        {
            uint32_t mt = find_memory_type(dev, memreq.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VkMemoryAllocateInfo mai;
            if (mt == UINT32_MAX) {
                fprintf(stderr, "mathgfx-vk: no host-visible coherent memory type\n");
                return 1;
            }
            memset(&mai, 0, sizeof(mai));
            mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            mai.allocationSize = memreq.size;
            mai.memoryTypeIndex = mt;
            VKCHK(vkAllocateMemory(device, &mai, NULL, &memory));
            VKCHK(vkBindBufferMemory(device, buffer, memory, 0));
            VKCHK(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped));
            memset(mapped, 0, (size_t)bytes);
        }

        memset(&binding, 0, sizeof(binding));
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        memset(&slci, 0, sizeof(slci));
        slci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        slci.bindingCount = 1;
        slci.pBindings = &binding;
        VKCHK(vkCreateDescriptorSetLayout(device, &slci, NULL, &set_layout));

        memset(&psize, 0, sizeof(psize));
        psize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        psize.descriptorCount = 1;
        memset(&dpci, 0, sizeof(dpci));
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = 1;
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes = &psize;
        VKCHK(vkCreateDescriptorPool(device, &dpci, NULL, &pool));
        memset(&dsai, 0, sizeof(dsai));
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = pool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &set_layout;
        VKCHK(vkAllocateDescriptorSets(device, &dsai, &set));

        memset(&dbi, 0, sizeof(dbi));
        dbi.buffer = buffer;
        dbi.offset = 0;
        dbi.range = VK_WHOLE_SIZE;
        memset(&write, 0, sizeof(write));
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &dbi;
        vkUpdateDescriptorSets(device, 1, &write, 0, NULL);

        memset(&pcr, 0, sizeof(pcr));
        pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcr.offset = 0;
        pcr.size = 56;  /* 14 * 4 bytes for PushParams (aligned) */
        memset(&plci, 0, sizeof(plci));
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &set_layout;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &pcr;
        VKCHK(vkCreatePipelineLayout(device, &plci, NULL, &pipeline_layout));

        {
            FILE *sf = fopen(shader_path, "rb");
            long n;
            if (!sf) {
                fprintf(stderr, "mathgfx-vk: cannot open shader '%s'\n", shader_path);
                return 1;
            }
            if (fseek(sf, 0, SEEK_END) != 0 || (n = ftell(sf)) <= 0 ||
                fseek(sf, 0, SEEK_SET) != 0) {
                fclose(sf);
                fprintf(stderr, "mathgfx-vk: cannot size shader '%s'\n", shader_path);
                return 1;
            }
            spv = (unsigned char *)malloc((size_t)n);
            if (!spv || fread(spv, 1, (size_t)n, sf) != (size_t)n) {
                free(spv);
                fclose(sf);
                fprintf(stderr, "mathgfx-vk: cannot read shader '%s'\n", shader_path);
                return 1;
            }
            fclose(sf);
            spv_size = (size_t)n;
            if (spv_size % 4 != 0) {
                free(spv);
                fprintf(stderr, "mathgfx-vk: shader size is not a multiple of 4\n");
                return 1;
            }
        }
        memset(&smci, 0, sizeof(smci));
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = spv_size;
        smci.pCode = (const uint32_t *)spv;
        VKCHK(vkCreateShaderModule(device, &smci, NULL, &shader));
        free(spv);
        spv = NULL;

        memset(&cpci, 0, sizeof(cpci));
        cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpci.layout = pipeline_layout;
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = shader;
        cpci.stage.pName = "main";
        VKCHK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci,
                                       NULL, &pipeline));

        memset(&cpci2, 0, sizeof(cpci2));
        cpci2.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpci2.queueFamilyIndex = qfamily;
        cpci2.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        VKCHK(vkCreateCommandPool(device, &cpci2, NULL, &cmd_pool));

        memset(&cbai, 0, sizeof(cbai));
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = cmd_pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        VKCHK(vkAllocateCommandBuffers(device, &cbai, &cmd));

        memset(&fci, 0, sizeof(fci));
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VKCHK(vkCreateFence(device, &fci, NULL, &fence));

        memset(&submit, 0, sizeof(submit));
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;

        PushParams pp;
        pp.w = (uint32_t)rw;
        pp.h = (uint32_t)rh;
        pp.time = time_val;
        pp.frame = 0;
        pp.aperture = aperture;
        pp.focus = focus;
        pp.samples = (uint32_t)dof_samples;
        pp.seed = 0;
        pp.loop_period = loop_period;
        pp.rigid_rotation = rigid_rotation ? 1u : 0u;
        pp.duration = duration > 0.0f ? duration : (time_end > time_val ? time_end - time_val : 0.0f);
        pp.total_frames = (uint32_t)frames;

        src = (uint32_t *)mapped;
        snap = (uint32_t *)malloc((size_t)rw * (size_t)rh * 4u);
        rgb = (unsigned char *)malloc((size_t)width * (size_t)height * 3u);
        if (!snap || !rgb) {
            fprintf(stderr, "mathgfx-vk: out of memory\n");
            free(snap);
            free(rgb);
            return 1;
        }

        t0 = mgfx_now_sec();
        if (video_path) {
            VideoWriter *vw;
            int f;
            double t_gpu = 0.0, t_ds = 0.0, t_enc = 0.0;
            if (frames <= 0) {
                fprintf(stderr, "mathgfx-vk: --video requires --frames N\n");
                free(rgb);
                return 1;
            }
            if (mgfx_ensure_parent_dir(video_path) != 0) {
                fprintf(stderr, "mathgfx-vk: cannot create directory for '%s'\n",
                        video_path);
                free(rgb);
                return 1;
            }
            vw = video_writer_open(video_path, width, height, fps, quality,
                                   crf, err, sizeof(err));
            if (!vw) {
                fprintf(stderr, "mathgfx-vk: %s\n", err);
                free(rgb);
                return 1;
            }
            for (f = 0; f < frames; f++) {
                float t;
                if (time_end >= 0.0f && frames > 1) {
                    t = time_val + (time_end - time_val) * (float)f / (float)(frames - 1);
                } else {
                    t = time_val + (float)f / (float)fps;
                }
                double a, b;
                a = mgfx_now_sec();
                pp.time = t;
                pp.frame = (uint32_t)f;
                if (focus_end > 0.0f && frames > 1) {
                    float u = 0.5f - 0.5f * cosf(6.2831853f * (float)f /
                                                 (float)(frames - 1));
                    pp.focus = focus + (focus_end - focus) * u;
                }
                record_dispatch(cmd, pipeline, pipeline_layout, set, buffer, &pp);
                VKCHK(vkResetFences(device, 1, &fence));
                VKCHK(vkQueueSubmit(queue, 1, &submit, fence));
                VKCHK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
                b = mgfx_now_sec();
                t_gpu += b - a;
                a = mgfx_now_sec();
                memcpy(snap, src, (size_t)rw * (size_t)rh * 4u);
                downsample_rgb(snap, rw, width, height, ssaa, rgb);
                b = mgfx_now_sec();
                t_ds += b - a;
                a = mgfx_now_sec();
                if (video_writer_add(vw, rgb, err, sizeof(err)) != 0) {
                    fprintf(stderr, "mathgfx-vk: %s\n", err);
                    video_writer_abort(vw);
                    free(rgb);
                    return 1;
                }
                b = mgfx_now_sec();
                t_enc += b - a;
            }
            if (video_writer_close(vw, err, sizeof(err)) != 0) {
                fprintf(stderr, "mathgfx-vk: %s\n", err);
                free(rgb);
                return 1;
            }
            t1 = mgfx_now_sec();
            printf("vulkan: %s, %d frames %dx%d @ %d fps (ssaa %d, dof %d, "
                   "aperture %.3f, focus %.1f) -> %s (%.3fs)\n",
                   gpu_name, frames, width, height, fps, ssaa, dof_samples,
                   aperture, focus, video_path, t1 - t0);
            printf("timing: gpu %.2fs, readback/downsample %.2fs, encode %.2fs\n",
                   t_gpu, t_ds, t_enc);
        } else {
            pp.time = time_val;
            pp.frame = 0;
            record_dispatch(cmd, pipeline, pipeline_layout, set, buffer, &pp);
            VKCHK(vkResetFences(device, 1, &fence));
            VKCHK(vkQueueSubmit(queue, 1, &submit, fence));
            VKCHK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
            t1 = mgfx_now_sec();
            memcpy(snap, src, (size_t)rw * (size_t)rh * 4u);
            downsample_rgb(snap, rw, width, height, ssaa, rgb);
            if (!mgfx_write_png(out_path, width, height, rgb)) {
                fprintf(stderr, "mathgfx-vk: cannot write '%s'\n", out_path);
                free(rgb);
                return 1;
            }
            printf("vulkan: %s, %dx%d (ssaa %d, dof %d, aperture %.3f, "
                   "focus %.1f) -> %s (%.3fs)\n",
                   gpu_name, width, height, ssaa, dof_samples,
                   aperture, focus, out_path, t1 - t0);
        }
        free(snap);
        free(rgb);

        vkUnmapMemory(device, memory);
        vkDestroyFence(device, fence, NULL);
        vkDestroyCommandPool(device, cmd_pool, NULL);
        vkDestroyPipeline(device, pipeline, NULL);
        vkDestroyShaderModule(device, shader, NULL);
        vkDestroyPipelineLayout(device, pipeline_layout, NULL);
        vkDestroyDescriptorPool(device, pool, NULL);
        vkDestroyDescriptorSetLayout(device, set_layout, NULL);
        vkDestroyBuffer(device, buffer, NULL);
        vkFreeMemory(device, memory, NULL);
        vkDestroyDevice(device, NULL);
        free(devs);
        vkDestroyInstance(instance, NULL);
    }

    return 0;
}
