/*
 * Spektrafilm for Android — GPU (Vulkan compute) fast-path implementation. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Compiled only when SPK_ENABLE_VULKAN is defined (see gpu/vulkan_compute.h and
 * CMakeLists.txt). Headless compute: instance -> physical device + compute queue ->
 * host-visible storage buffers -> compute pipeline (embedded SPIR-V) -> dispatch ->
 * read back. Every failure path returns false so the caller falls back to the CPU.
 *
 * PERSISTENT HOST (GPU M1, #146): the scan kernel's pipeline, descriptor set,
 * command buffer and buffers are created once and reused across calls (buffers
 * grow-only), instead of being rebuilt per dispatch. The PR #145 device probe
 * measured the old per-call host at ~25-48 ms of fixed overhead per dispatch —
 * killing that cost is the preview-tier win. The vendored SPIR-V is UNCHANGED, so
 * the probe's published Tier 1 error numbers still describe the shipped binary.
 * Calls are serialized by a mutex (one queue, one command buffer); on any Vulkan
 * failure the kernel state is torn down and the call returns false (CPU fallback),
 * so a later call can retry from scratch.
 *
 * NaN GUARD (#145 caveat, mandated by #146): GLSL clamp(NaN) is implementation-
 * defined, so the engine's NaN-density -> black semantics must not rest on driver
 * behaviour. The input upload maps every non-finite density component to 1e4f
 * (10^-1e4 underflows to exactly 0 in fp32 -> zero transmittance -> black), which
 * is deterministic on every conformant driver. Finite inputs are copied verbatim,
 * so the probe's error measurements are unaffected.
 */
#include "gpu/vulkan_compute.h"

#ifndef SPK_ENABLE_VULKAN

namespace spk::gpu {
bool available() { return false; }
bool cctf_encode_srgb(float*, size_t) { return false; }
bool scan_spectral(const float*, float*, uint32_t, const float*, const float*, const float*) { return false; }
bool scan_spectral_linear(const float*, float*, uint32_t, const float*, const float*, const float*) { return false; }
}  // namespace spk::gpu

#else

#include <vulkan/vulkan.h>

#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "gpu/cctf_encode_spv.h"
#include "gpu/scan_spectral_lin_spv.h"
#include "gpu/scan_spectral_spv.h"

namespace spk::gpu {
namespace {

// One host-visible storage buffer + its memory + its persistent mapping.
struct Buf {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize cap = 0;
};

struct Ctx {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    bool ok = false;

    // Persistent per-kernel state (built lazily on the kernel's first call).
    // The full-chain (scan_spectral.comp) and linear (scan_spectral_lin.comp)
    // kernels share this shape — identical bindings + push-constant layout —
    // but keep separate pipelines and buffers.
    struct Kernel {
        VkShaderModule shader = VK_NULL_HANDLE;
        VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
        VkPipelineLayout pl = VK_NULL_HANDLE;
        VkPipeline pipe = VK_NULL_HANDLE;
        VkDescriptorPool dpool = VK_NULL_HANDLE;
        VkDescriptorSet dset = VK_NULL_HANDLE;  // freed with dpool
        VkCommandPool cpool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;   // freed with cpool
        VkFence fence = VK_NULL_HANDLE;
        Buf in, out, dyeB, cmfB;                // in/out grow-only; tables fixed
        bool pipelineReady = false;
    };
    Kernel scanFused;  // scan_spectral.comp (density -> encoded sRGB)
    Kernel scanLin;    // scan_spectral_lin.comp (density -> unclipped linear RGB)

    bool init() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "spektra";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) return false;

        uint32_t nphys = 0;
        vkEnumeratePhysicalDevices(instance, &nphys, nullptr);
        if (nphys == 0) return false;
        std::vector<VkPhysicalDevice> devs(nphys);
        vkEnumeratePhysicalDevices(instance, &nphys, devs.data());
        phys = devs[0];

        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qprops.data());
        bool found = false;
        for (uint32_t i = 0; i < nq; ++i) {
            if (qprops[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { queueFamily = i; found = true; break; }
        }
        if (!found) return false;

        float pri = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = queueFamily;
        qci.queueCount = 1;
        qci.pQueuePriorities = &pri;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        if (vkCreateDevice(phys, &dci, nullptr, &device) != VK_SUCCESS) return false;
        vkGetDeviceQueue(device, queueFamily, 0, &queue);
        ok = true;
        return true;
    }

    void destroyBuf(Buf& b) {
        if (b.mapped) { vkUnmapMemory(device, b.mem); b.mapped = nullptr; }
        if (b.mem) { vkFreeMemory(device, b.mem, nullptr); b.mem = VK_NULL_HANDLE; }
        if (b.buf) { vkDestroyBuffer(device, b.buf, nullptr); b.buf = VK_NULL_HANDLE; }
        b.cap = 0;
    }

    // Tear down all persistent kernel state (failure recovery only — never runs
    // at process exit, see the deliberate leak below).
    void destroyKernel(Kernel& s) {
        if (s.fence) { vkDestroyFence(device, s.fence, nullptr); s.fence = VK_NULL_HANDLE; }
        if (s.cpool) { vkDestroyCommandPool(device, s.cpool, nullptr); s.cpool = VK_NULL_HANDLE; s.cmd = VK_NULL_HANDLE; }
        if (s.dpool) { vkDestroyDescriptorPool(device, s.dpool, nullptr); s.dpool = VK_NULL_HANDLE; s.dset = VK_NULL_HANDLE; }
        if (s.pipe) { vkDestroyPipeline(device, s.pipe, nullptr); s.pipe = VK_NULL_HANDLE; }
        if (s.pl) { vkDestroyPipelineLayout(device, s.pl, nullptr); s.pl = VK_NULL_HANDLE; }
        if (s.dsl) { vkDestroyDescriptorSetLayout(device, s.dsl, nullptr); s.dsl = VK_NULL_HANDLE; }
        if (s.shader) { vkDestroyShaderModule(device, s.shader, nullptr); s.shader = VK_NULL_HANDLE; }
        destroyBuf(s.in);
        destroyBuf(s.out);
        destroyBuf(s.dyeB);
        destroyBuf(s.cmfB);
        s.pipelineReady = false;
    }

    void destroyScan() {
        if (!device) return;
        vkDeviceWaitIdle(device);
        destroyKernel(scanFused);
        destroyKernel(scanLin);
    }

    int findMemType(uint32_t bits, VkMemoryPropertyFlags want) {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(phys, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
        return -1;
    }

    // Create (or grow) a HOST_VISIBLE|COHERENT storage buffer and keep it mapped.
    // Same memory type as the per-call host the probe validated; only the lifetime
    // changed. Returns false on any Vulkan failure.
    bool ensureBuf(Buf& b, VkDeviceSize bytes) {
        if (b.cap >= bytes && b.buf) return true;
        destroyBuf(b);
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bci, nullptr, &b.buf) != VK_SUCCESS) return false;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, b.buf, &req);
        int mt = findMemType(req.memoryTypeBits,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mt < 0) return false;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = (uint32_t)mt;
        if (vkAllocateMemory(device, &mai, nullptr, &b.mem) != VK_SUCCESS) return false;
        if (vkBindBufferMemory(device, b.buf, b.mem, 0) != VK_SUCCESS) return false;
        if (vkMapMemory(device, b.mem, 0, VK_WHOLE_SIZE, 0, &b.mapped) != VK_SUCCESS) return false;
        b.cap = bytes;
        return true;
    }
};

// One lazily-initialised context for the process, plus the lock serializing all
// GPU entry points (one queue + one reusable command buffer).
//
// The context is DELIBERATELY LEAKED (new, never deleted): running Vulkan
// teardown from a static destructor at process exit crashes on ICDs whose own
// statics unload first (observed as a SIGSEGV under SwiftShader), and Android
// kills app processes without running static destructors anyway. destroyScan()
// exists for mid-process FAILURE recovery, where the device is alive and the
// calls are safe.
std::mutex& gpu_mutex() { static std::mutex m; return m; }
Ctx& ctx() {
    static Ctx* c = nullptr;
    if (!c) { c = new Ctx(); c->init(); }
    return *c;
}

struct ScanPush { uint32_t npix; float m[12]; };  // std430 push constant (matches both shaders)

// Build one kernel's persistent pipeline objects (everything except the
// grow-only image buffers) from its SPIR-V blob. Called once per kernel; on
// failure the caller tears down via destroyScan().
bool build_scan_pipeline(Ctx& c, Ctx::Kernel& s, const uint32_t* spv, size_t spvBytes) {
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = spvBytes;
    smci.pCode = spv;
    if (vkCreateShaderModule(c.device, &smci, nullptr, &s.shader) != VK_SUCCESS) return false;

    VkDescriptorSetLayoutBinding b[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dlci.bindingCount = 4;
    dlci.pBindings = b;
    if (vkCreateDescriptorSetLayout(c.device, &dlci, nullptr, &s.dsl) != VK_SUCCESS) return false;

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ScanPush)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &s.dsl;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(c.device, &plci, nullptr, &s.pl) != VK_SUCCESS) return false;

    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = s.shader;
    cpci.stage.pName = "main";
    cpci.layout = s.pl;
    if (vkCreateComputePipelines(c.device, VK_NULL_HANDLE, 1, &cpci, nullptr, &s.pipe) != VK_SUCCESS) return false;

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(c.device, &dpci, nullptr, &s.dpool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = s.dpool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &s.dsl;
    if (vkAllocateDescriptorSets(c.device, &dsai, &s.dset) != VK_SUCCESS) return false;

    VkCommandPoolCreateInfo cpci2{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci2.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci2.queueFamilyIndex = c.queueFamily;
    if (vkCreateCommandPool(c.device, &cpci2, nullptr, &s.cpool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = s.cpool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(c.device, &cbai, &s.cmd) != VK_SUCCESS) return false;

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(c.device, &fci, nullptr, &s.fence) != VK_SUCCESS) return false;

    s.pipelineReady = true;
    return true;
}

}  // namespace

bool available() {
    std::lock_guard<std::mutex> lk(gpu_mutex());
    return ctx().ok;
}

bool cctf_encode_srgb(float* data, size_t n) {
    std::lock_guard<std::mutex> lk(gpu_mutex());
    Ctx& c = ctx();
    if (!c.ok || data == nullptr || n == 0) return false;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(n) * sizeof(float);
    bool ok = false;

    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkCommandPool cpool = VK_NULL_HANDLE;

    do {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(c.device, &bci, nullptr, &buf) != VK_SUCCESS) break;

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(c.device, buf, &req);
        int mt = c.findMemType(req.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mt < 0) break;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = (uint32_t)mt;
        if (vkAllocateMemory(c.device, &mai, nullptr, &mem) != VK_SUCCESS) break;
        vkBindBufferMemory(c.device, buf, mem, 0);

        // Upload.
        void* mapped = nullptr;
        if (vkMapMemory(c.device, mem, 0, bytes, 0, &mapped) != VK_SUCCESS) break;
        std::memcpy(mapped, data, bytes);
        vkUnmapMemory(c.device, mem);

        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = sizeof(kCctfEncodeSpv);
        smci.pCode = kCctfEncodeSpv;
        if (vkCreateShaderModule(c.device, &smci, nullptr, &shader) != VK_SUCCESS) break;

        VkDescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dlci.bindingCount = 1;
        dlci.pBindings = &b;
        if (vkCreateDescriptorSetLayout(c.device, &dlci, nullptr, &dsl) != VK_SUCCESS) break;

        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t)};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &dsl;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &pcr;
        if (vkCreatePipelineLayout(c.device, &plci, nullptr, &pl) != VK_SUCCESS) break;

        VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = shader;
        cpci.stage.pName = "main";
        cpci.layout = pl;
        if (vkCreateComputePipelines(c.device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipe) != VK_SUCCESS) break;

        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpci.maxSets = 1;
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(c.device, &dpci, nullptr, &dpool) != VK_SUCCESS) break;
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = dpool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &dsl;
        VkDescriptorSet dset = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(c.device, &dsai, &dset) != VK_SUCCESS) break;
        VkDescriptorBufferInfo dbi{buf, 0, bytes};
        VkWriteDescriptorSet wds{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wds.dstSet = dset;
        wds.descriptorCount = 1;
        wds.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        wds.pBufferInfo = &dbi;
        vkUpdateDescriptorSets(c.device, 1, &wds, 0, nullptr);

        VkCommandPoolCreateInfo cpci2{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpci2.queueFamilyIndex = c.queueFamily;
        if (vkCreateCommandPool(c.device, &cpci2, nullptr, &cpool) != VK_SUCCESS) break;
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = cpool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(c.device, &cbai, &cmd) != VK_SUCCESS) break;

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &dset, 0, nullptr);
        uint32_t count = static_cast<uint32_t>(n);
        vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t), &count);
        vkCmdDispatch(cmd, (count + 63) / 64, 1, 1);
        vkEndCommandBuffer(cmd);

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        if (vkQueueSubmit(c.queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) break;
        vkQueueWaitIdle(c.queue);

        // Read back.
        if (vkMapMemory(c.device, mem, 0, bytes, 0, &mapped) != VK_SUCCESS) break;
        std::memcpy(data, mapped, bytes);
        vkUnmapMemory(c.device, mem);
        ok = true;
    } while (false);

    if (cpool) vkDestroyCommandPool(c.device, cpool, nullptr);
    if (dpool) vkDestroyDescriptorPool(c.device, dpool, nullptr);
    if (pipe) vkDestroyPipeline(c.device, pipe, nullptr);
    if (pl) vkDestroyPipelineLayout(c.device, pl, nullptr);
    if (dsl) vkDestroyDescriptorSetLayout(c.device, dsl, nullptr);
    if (shader) vkDestroyShaderModule(c.device, shader, nullptr);
    if (mem) vkFreeMemory(c.device, mem, nullptr);
    if (buf) vkDestroyBuffer(c.device, buf, nullptr);
    return ok;
}

// Shared dispatch body for the two scan kernels (mutex held by the callers).
static bool dispatch_scan(Ctx& c, Ctx::Kernel& s, const uint32_t* spv, size_t spvBytes,
                          const float* cmy, float* rgb, uint32_t npix,
                          const float* dye, const float* icmf, const float* xyz2rgb) {
    if (!c.ok || !cmy || !rgb || npix == 0 || !dye || !icmf || !xyz2rgb) return false;
    const int NB = 81;
    const VkDeviceSize tblBytes = static_cast<VkDeviceSize>(NB) * 3u * sizeof(float);
    // SLICING (GPU export, #154): a single dispatch is capped at the
    // spec-guaranteed maxComputeWorkGroupCount floor (65535 groups × 64 =
    // 4,193,280 px). Full-res exports (a 12.5 MP frame) exceed that, so the
    // image is processed in slices of at most MAX_SLICE pixels. The persistent
    // in/out buffers are sized to the SLICE, not the whole image, bounding GPU
    // memory. A preview (npix < MAX_SLICE) is exactly one slice — identical to
    // the pre-slicing single dispatch, so the PR #145 numbers still hold.
    const uint32_t MAX_SLICE = 65535u * 64u;  // 4,193,280
    const uint32_t sliceCap = npix < MAX_SLICE ? npix : MAX_SLICE;
    const VkDeviceSize sliceBytes = static_cast<VkDeviceSize>(sliceCap) * 3u * sizeof(float);
    bool ok = false;

    do {
        if (!s.pipelineReady && !build_scan_pipeline(c, s, spv, spvBytes)) break;

        // Grow-only slice buffers; fixed-size tables. Any (re)creation requires a
        // descriptor rewrite; buffers only change while the queue is idle (each
        // slice's fence is waited before the next), so rewriting descriptors
        // here is race-free under the mutex.
        const bool hadIn = s.in.cap >= sliceBytes && s.in.buf;
        const bool hadOut = s.out.cap >= sliceBytes && s.out.buf;
        if (!c.ensureBuf(s.in, sliceBytes)) break;
        if (!c.ensureBuf(s.out, sliceBytes)) break;
        if (!c.ensureBuf(s.dyeB, tblBytes)) break;
        if (!c.ensureBuf(s.cmfB, tblBytes)) break;
        if (!hadIn || !hadOut) {
            VkBuffer bufs[4] = {s.in.buf, s.out.buf, s.dyeB.buf, s.cmfB.buf};
            VkDeviceSize caps[4] = {s.in.cap, s.out.cap, s.dyeB.cap, s.cmfB.cap};
            VkDescriptorBufferInfo dbi[4];
            VkWriteDescriptorSet wds[4];
            for (uint32_t i = 0; i < 4; ++i) {
                dbi[i] = VkDescriptorBufferInfo{bufs[i], 0, caps[i]};
                wds[i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                wds[i].dstSet = s.dset;
                wds[i].dstBinding = i;
                wds[i].descriptorCount = 1;
                wds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                wds[i].pBufferInfo = &dbi[i];
            }
            vkUpdateDescriptorSets(c.device, 4, wds, 0, nullptr);
        }

        // Tables are tiny (~1 KB) and constant across slices; upload once.
        std::memcpy(s.dyeB.mapped, dye, tblBytes);
        std::memcpy(s.cmfB.mapped, icmf, tblBytes);

        ScanPush push{};
        // Pack the row-major 3x3 XYZ->RGB into the shader's 3x4 layout (m[0..2],[4..6],[8..10]).
        push.m[0] = xyz2rgb[0]; push.m[1] = xyz2rgb[1]; push.m[2]  = xyz2rgb[2]; push.m[3]  = 0.0f;
        push.m[4] = xyz2rgb[3]; push.m[5] = xyz2rgb[4]; push.m[6]  = xyz2rgb[5]; push.m[7]  = 0.0f;
        push.m[8] = xyz2rgb[6]; push.m[9] = xyz2rgb[7]; push.m[10] = xyz2rgb[8]; push.m[11] = 0.0f;

        bool slice_ok = true;
        for (uint32_t base = 0; base < npix && slice_ok; base += MAX_SLICE) {
            const uint32_t n = (npix - base) < MAX_SLICE ? (npix - base) : MAX_SLICE;
            const size_t ncomp = static_cast<size_t>(n) * 3u;

            // Upload this slice. The input copy is the NaN guard: non-finite
            // densities map to 1e4f (zero transmittance -> black) so the shader
            // never sees a NaN/Inf (clamp(NaN) is implementation-defined in
            // GLSL). Finite inputs are copied verbatim (bit-exact).
            float* dst = static_cast<float*>(s.in.mapped);
            const float* src = cmy + static_cast<size_t>(base) * 3u;
            for (size_t i = 0; i < ncomp; ++i) {
                const float v = src[i];
                dst[i] = std::isfinite(v) ? v : 1e4f;
            }

            // Record + submit. The command buffer is reused; each slice's fence
            // is waited before the buffer is reset again.
            if (vkResetCommandBuffer(s.cmd, 0) != VK_SUCCESS) { slice_ok = false; break; }
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (vkBeginCommandBuffer(s.cmd, &bi) != VK_SUCCESS) { slice_ok = false; break; }
            vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipe);
            vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pl, 0, 1, &s.dset, 0, nullptr);
            push.npix = n;
            vkCmdPushConstants(s.cmd, s.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ScanPush), &push);
            vkCmdDispatch(s.cmd, (n + 63u) / 64u, 1, 1);
            if (vkEndCommandBuffer(s.cmd) != VK_SUCCESS) { slice_ok = false; break; }

            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &s.cmd;
            if (vkQueueSubmit(c.queue, 1, &si, s.fence) != VK_SUCCESS) { slice_ok = false; break; }
            if (vkWaitForFences(c.device, 1, &s.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) { slice_ok = false; break; }
            if (vkResetFences(c.device, 1, &s.fence) != VK_SUCCESS) { slice_ok = false; break; }

            // Read this slice back (persistently mapped, HOST_COHERENT).
            std::memcpy(rgb + static_cast<size_t>(base) * 3u, s.out.mapped,
                        ncomp * sizeof(float));
        }
        ok = slice_ok;
    } while (false);

    // Failure recovery: tear the persistent state down so the next call rebuilds
    // from scratch (and the caller falls back to the CPU for this frame).
    if (!ok) c.destroyScan();
    return ok;
}

bool scan_spectral(const float* cmy, float* rgb, uint32_t npix,
                   const float* dye, const float* icmf, const float* xyz2rgb) {
    std::lock_guard<std::mutex> lk(gpu_mutex());
    Ctx& c = ctx();
    return dispatch_scan(c, c.scanFused, kScanSpectralSpv, sizeof(kScanSpectralSpv),
                         cmy, rgb, npix, dye, icmf, xyz2rgb);
}

bool scan_spectral_linear(const float* cmy, float* rgb, uint32_t npix,
                          const float* dye, const float* icmf, const float* xyz2rgb) {
    std::lock_guard<std::mutex> lk(gpu_mutex());
    Ctx& c = ctx();
    return dispatch_scan(c, c.scanLin, kScanSpectralLinSpv, sizeof(kScanSpectralLinSpv),
                         cmy, rgb, npix, dye, icmf, xyz2rgb);
}

}  // namespace spk::gpu

#endif  // SPK_ENABLE_VULKAN
