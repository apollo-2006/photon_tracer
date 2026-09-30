#pragma once
// A thin layer over Vulkan for the GPU renderer: one device and queue,
// buffers with device addresses, images, and one-shot command buffers. No
// window: everything renders to memory and is read back.
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#define VK_CHECK(call)                                                                          \
    do {                                                                                        \
        const VkResult result_ = (call);                                                        \
        if (result_ != VK_SUCCESS)                                                              \
            throw std::runtime_error(std::string(#call) + " failed: VkResult " + std::to_string(result_)); \
    } while (0)

namespace vk {

// Validation errors reported through the debug messenger. Counted, so a run
// with --validate fails if the layers found anything.
inline int validation_errors = 0;

inline VKAPI_ATTR VkBool32 VKAPI_CALL on_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                 VkDebugUtilsMessageTypeFlagsEXT,
                                                 const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::fprintf(stderr, "validation: %s\n", data->pMessage);
        if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++validation_errors;
    }
    return VK_FALSE;
}

struct buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    VkDeviceSize size = 0;
    void* mapped = nullptr;  // For host-visible buffers
};

struct image {
    VkImage handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

class context {
public:
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    std::string device_name;

    // Extension entry points.
    PFN_vkCmdDrawMeshTasksEXT draw_mesh_tasks = nullptr;
    PFN_vkCreateAccelerationStructureKHR create_as = nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroy_as = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR as_build_sizes = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR build_as = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR as_address = nullptr;

    explicit context(bool validate) {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "photon_tracer_gpu";
        app.apiVersion = VK_API_VERSION_1_3;
        std::vector<const char*> layers, extensions;
        if (validate) {
            layers.push_back("VK_LAYER_KHRONOS_validation");
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        ici.enabledLayerCount = static_cast<uint32_t>(layers.size());
        ici.ppEnabledLayerNames = layers.data();
        ici.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        ici.ppEnabledExtensionNames = extensions.data();
        VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));
        if (validate) {
            VkDebugUtilsMessengerCreateInfoEXT mci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            mci.pfnUserCallback = on_message;
            auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
            if (create) VK_CHECK(create(instance, &mci, nullptr, &messenger));
        }
        pick_device();
        create_device();
    }

    ~context() {
        if (device) {
            vkDeviceWaitIdle(device);
            vkDestroyCommandPool(device, pool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (messenger) {
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy) destroy(instance, messenger, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }

    context(const context&) = delete;
    context& operator=(const context&) = delete;

    // A buffer with a device address. Host-visible buffers stay mapped.
    buffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible) {
        buffer b;
        b.size = size;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size ? size : 4;
        bci.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        VK_CHECK(vkCreateBuffer(device, &bci, nullptr, &b.handle));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, b.handle, &req);
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.pNext = &flags;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = memory_type(req.memoryTypeBits, host_visible
            ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
            : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &b.memory));
        VK_CHECK(vkBindBufferMemory(device, b.handle, b.memory, 0));
        if (host_visible) VK_CHECK(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped));
        VkBufferDeviceAddressInfo bai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        bai.buffer = b.handle;
        b.address = vkGetBufferDeviceAddress(device, &bai);
        return b;
    }

    // A device-local buffer holding bytes, copied in through a staging buffer.
    buffer upload(const void* data, VkDeviceSize size, VkBufferUsageFlags usage) {
        buffer b = make_buffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
        if (size == 0) return b;
        buffer staging = make_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        std::memcpy(staging.mapped, data, size);
        submit([&](VkCommandBuffer cmd) {
            VkBufferCopy copy{0, 0, size};
            vkCmdCopyBuffer(cmd, staging.handle, b.handle, 1, &copy);
        });
        destroy(staging);
        return b;
    }
    template <class T>
    buffer upload(const std::vector<T>& v, VkBufferUsageFlags usage) {
        return upload(v.data(), v.size() * sizeof(T), usage);
    }

    image make_image(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
        image im;
        VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ic.imageType = VK_IMAGE_TYPE_2D;
        ic.format = format;
        ic.extent = {w, h, 1};
        ic.mipLevels = 1;
        ic.arrayLayers = 1;
        ic.samples = VK_SAMPLE_COUNT_1_BIT;
        ic.tiling = VK_IMAGE_TILING_OPTIMAL;
        ic.usage = usage;
        VK_CHECK(vkCreateImage(device, &ic, nullptr, &im.handle));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, im.handle, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &im.memory));
        VK_CHECK(vkBindImageMemory(device, im.handle, im.memory, 0));
        VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vc.image = im.handle;
        vc.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vc.format = format;
        vc.subresourceRange = {aspect, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device, &vc, nullptr, &im.view));
        return im;
    }

    void destroy(buffer& b) {
        if (b.mapped) vkUnmapMemory(device, b.memory);
        vkDestroyBuffer(device, b.handle, nullptr);
        vkFreeMemory(device, b.memory, nullptr);
        b = buffer{};
    }
    void destroy(image& im) {
        vkDestroyImageView(device, im.view, nullptr);
        vkDestroyImage(device, im.handle, nullptr);
        vkFreeMemory(device, im.memory, nullptr);
        im = image{};
    }

    // Records commands with record(cmd), submits them and waits.
    template <class Record>
    void submit(Record&& record) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd;
        VK_CHECK(vkAllocateCommandBuffers(device, &ai, &cmd));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
        record(cmd);
        VK_CHECK(vkEndCommandBuffer(cmd));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        VK_CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(queue));
        vkFreeCommandBuffers(device, pool, 1, &cmd);
    }

    VkShaderModule shader(const uint32_t* code, size_t bytes) {
        VkShaderModuleCreateInfo sci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sci.codeSize = bytes;
        sci.pCode = code;
        VkShaderModule m;
        VK_CHECK(vkCreateShaderModule(device, &sci, nullptr, &m));
        return m;
    }

private:
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;

    static constexpr const char* required[] = {
        VK_EXT_MESH_SHADER_EXTENSION_NAME,
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_QUERY_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    };

    // The first GPU with Vulkan 1.3, mesh shaders and ray queries; discrete first.
    void pick_device() {
        uint32_t n = 0;
        vkEnumeratePhysicalDevices(instance, &n, nullptr);
        std::vector<VkPhysicalDevice> gpus(n);
        vkEnumeratePhysicalDevices(instance, &n, gpus.data());
        VkPhysicalDevice fallback = VK_NULL_HANDLE;
        for (VkPhysicalDevice d : gpus) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(d, &p);
            if (p.apiVersion < VK_API_VERSION_1_3 || !has_extensions(d)) continue;
            if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { gpu = d; break; }
            if (!fallback) fallback = d;
        }
        if (!gpu) gpu = fallback;
        if (!gpu) throw std::runtime_error("no GPU with Vulkan 1.3, VK_EXT_mesh_shader and VK_KHR_ray_query");
        vkGetPhysicalDeviceProperties(gpu, &properties);
        device_name = properties.deviceName;
    }

    static bool has_extensions(VkPhysicalDevice d) {
        uint32_t n = 0;
        vkEnumerateDeviceExtensionProperties(d, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> ext(n);
        vkEnumerateDeviceExtensionProperties(d, nullptr, &n, ext.data());
        for (const char* want : required) {
            bool found = false;
            for (const auto& e : ext) found |= std::strcmp(e.extensionName, want) == 0;
            if (!found) return false;
        }
        return true;
    }

    void create_device() {
        uint32_t n = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, nullptr);
        std::vector<VkQueueFamilyProperties> families(n);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, families.data());
        queue_family = UINT32_MAX;
        for (uint32_t i = 0; i < n; ++i)
            if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { queue_family = i; break; }
        if (queue_family == UINT32_MAX) throw std::runtime_error("no graphics and compute queue");

        const float priority = 1;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = queue_family;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;

        VkPhysicalDeviceRayQueryFeaturesKHR ray_query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        ray_query.rayQuery = VK_TRUE;
        VkPhysicalDeviceAccelerationStructureFeaturesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        as.accelerationStructure = VK_TRUE;
        as.pNext = &ray_query;
        VkPhysicalDeviceMeshShaderFeaturesEXT mesh{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
        mesh.taskShader = VK_TRUE;
        mesh.meshShader = VK_TRUE;
        mesh.pNext = &as;
        VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        v13.dynamicRendering = VK_TRUE;
        v13.maintenance4 = VK_TRUE;
        v13.pNext = &mesh;
        VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        v12.bufferDeviceAddress = VK_TRUE;
        v12.descriptorIndexing = VK_TRUE;
        v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        v12.scalarBlockLayout = VK_TRUE;
        v12.shaderBufferInt64Atomics = VK_TRUE;  // The ray counter
        v12.pNext = &v13;
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.features.shaderInt64 = VK_TRUE;
        features.features.shaderFloat64 = VK_TRUE;  // The ground sphere's intersection
        features.pNext = &v12;

        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.pNext = &features;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = static_cast<uint32_t>(std::size(required));
        dci.ppEnabledExtensionNames = required;
        VK_CHECK(vkCreateDevice(gpu, &dci, nullptr, &device));
        vkGetDeviceQueue(device, queue_family, 0, &queue);

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = queue_family;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK_CHECK(vkCreateCommandPool(device, &pci, nullptr, &pool));

        draw_mesh_tasks = reinterpret_cast<PFN_vkCmdDrawMeshTasksEXT>(vkGetDeviceProcAddr(device, "vkCmdDrawMeshTasksEXT"));
        create_as = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(vkGetDeviceProcAddr(device, "vkCreateAccelerationStructureKHR"));
        destroy_as = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(vkGetDeviceProcAddr(device, "vkDestroyAccelerationStructureKHR"));
        as_build_sizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
            vkGetDeviceProcAddr(device, "vkGetAccelerationStructureBuildSizesKHR"));
        build_as = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
            vkGetDeviceProcAddr(device, "vkCmdBuildAccelerationStructuresKHR"));
        as_address = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
            vkGetDeviceProcAddr(device, "vkGetAccelerationStructureDeviceAddressKHR"));
    }

    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
        throw std::runtime_error("no suitable memory type");
    }
};

}  // namespace vk
