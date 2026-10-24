#include "bridge.hpp"
#include <algorithm>
#include <chrono>
#include <bit>
#include <cstring>
#include <poll.h>
#include <cerrno>

namespace poc {
bool frontend_device_negotiated = false;
namespace {
// Dawn may release from COLOR_ATTACHMENT_OPTIMAL or TRANSFER_SRC_OPTIMAL.
// The bridge image must permit both layouts, even though it only copies pixels.
constexpr auto shared_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
constexpr auto output_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
constexpr const char* extensions[] = {VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
struct QueueLock {
    const retro_hw_render_interface_vulkan* v;
    explicit QueueLock(const retro_hw_render_interface_vulkan* p) : v(p) { v->lock_queue(v->handle); }
    ~QueueLock() { v->unlock_queue(v->handle); }
};
VkImageSubresourceRange range() { return {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; }
void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
             VkAccessFlags src, VkAccessFlags dst, uint32_t from = VK_QUEUE_FAMILY_IGNORED, uint32_t to = VK_QUEUE_FAMILY_IGNORED) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src; b.dstAccessMask = dst; b.oldLayout = old_layout; b.newLayout = new_layout;
    b.srcQueueFamilyIndex = from; b.dstQueueFamilyIndex = to; b.image = image; b.subresourceRange = range();
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
}
uint32_t memory_type(VkPhysicalDevice gpu, uint32_t bits, VkMemoryPropertyFlags preferred) {
    VkPhysicalDeviceMemoryProperties p{}; vkGetPhysicalDeviceMemoryProperties(gpu,&p);
    for (uint32_t i=0;i<p.memoryTypeCount;++i)
        if ((bits & (1u<<i)) && (p.memoryTypes[i].propertyFlags & preferred) == preferred) return i;
    for (uint32_t i=0;i<p.memoryTypeCount;++i) if (bits & (1u<<i)) return i;
    throw std::runtime_error("No compatible Vulkan memory type");
}
void wait_fd(int fd) {
    if (fd == -1) return; // SYNC_FD -1 is an already-signaled payload.
    pollfd p{fd,POLLIN,0}; int result;
    do { result = poll(&p,1,30000); } while (result < 0 && errno == EINTR);
    require(result == 1 && (p.revents & POLLIN) && !(p.revents & (POLLERR|POLLNVAL)), "SYNC_FD CPU wait failed/timed out");
}
}
void probe_gpu(VkPhysicalDevice gpu) {
    VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(gpu,&p);
    log("Frontend GPU: %s Vulkan=%u.%u.%u driver=0x%x vendor=%04x device=%04x type=%u",
        p.deviceName,VK_VERSION_MAJOR(p.apiVersion),VK_VERSION_MINOR(p.apiVersion),VK_VERSION_PATCH(p.apiVersion),p.driverVersion,p.vendorID,p.deviceID,p.deviceType);
    require(p.apiVersion >= VK_API_VERSION_1_1, "Vulkan 1.1 required");
    require(p.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU, "Software Vulkan device rejected");
    uint32_t n=0; VK_CHECK(vkEnumerateDeviceExtensionProperties(gpu,nullptr,&n,nullptr));
    std::vector<VkExtensionProperties> props(n); VK_CHECK(vkEnumerateDeviceExtensionProperties(gpu,nullptr,&n,props.data()));
    bool all=true;
    for (const char* name : extensions) {
        bool present = std::any_of(props.begin(),props.end(),[&](auto& e){return std::strcmp(e.extensionName,name)==0;});
        all &= present;
    }
    require(all,"Missing required frontend-device extension");
    VkPhysicalDeviceExternalImageFormatInfo external{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
    external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkPhysicalDeviceImageFormatInfo2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
    query.pNext=&external; query.format=VK_FORMAT_R8G8B8A8_UNORM; query.type=VK_IMAGE_TYPE_2D;
    query.tiling=VK_IMAGE_TILING_OPTIMAL; query.usage=shared_usage; query.flags=0;
    VkAndroidHardwareBufferUsageANDROID ahb_usage{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_USAGE_ANDROID};
    VkExternalImageFormatProperties ext_props{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES}; ext_props.pNext=&ahb_usage;
    VkImageFormatProperties2 result{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2}; result.pNext=&ext_props;
    VkResult r=vkGetPhysicalDeviceImageFormatProperties2(gpu,&query,&result);
    VK_CHECK(r);
    require(ext_props.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT,"AHB image not importable");
    require(result.imageFormatProperties.sampleCounts & VK_SAMPLE_COUNT_1_BIT,"Single-sample AHB unsupported");
    // Query the exact ordinary image tuple as well as its linear-filter feature.
    query.pNext=nullptr; query.usage=output_usage; query.flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    result.pNext=nullptr; VK_CHECK(vkGetPhysicalDeviceImageFormatProperties2(gpu,&query,&result));
    VkFormatProperties fp{}; vkGetPhysicalDeviceFormatProperties(gpu,VK_FORMAT_R8G8B8A8_UNORM,&fp);
    require(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,"RGBA8 output cannot be sampled linearly");
    VkPhysicalDeviceExternalSemaphoreInfo sem{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
    sem.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    VkExternalSemaphoreProperties sp{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    vkGetPhysicalDeviceExternalSemaphoreProperties(gpu,&sem,&sp);
    require((sp.externalSemaphoreFeatures & (VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT|VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT)) ==
        (VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT|VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT),"SYNC_FD import/export unsupported");
}
VkInstance create_instance(PFN_vkGetInstanceProcAddr, const VkApplicationInfo* app, retro_vulkan_create_instance_wrapper_t wrapper, void* opaque) {
    // The wrapper adds and preserves frontend WSI extensions and requirements.
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo=app;
    return wrapper(opaque,&ci);
}
bool create_device2(retro_vulkan_context* context, VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface,
                   PFN_vkGetInstanceProcAddr, retro_vulkan_create_device_wrapper_t wrapper, void* opaque) {
    frontend_device_negotiated=false;
    try {
        uint32_t count=0;
        std::vector<VkPhysicalDevice> gpus;
        if(gpu) gpus.push_back(gpu);
        else { VK_CHECK(vkEnumeratePhysicalDevices(instance,&count,nullptr)); gpus.resize(count); VK_CHECK(vkEnumeratePhysicalDevices(instance,&count,gpus.data())); }
        for(auto candidate:gpus) {
            try { probe_gpu(candidate); } catch(const std::exception& e) { log("GPU probe rejected: %s",e.what()); continue; }
            vkGetPhysicalDeviceQueueFamilyProperties(candidate,&count,nullptr);
            std::vector<VkQueueFamilyProperties> qs(count); vkGetPhysicalDeviceQueueFamilyProperties(candidate,&count,qs.data());
            for(uint32_t i=0;i<count;++i) {
                VkBool32 present=VK_TRUE;
                if(surface) VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(candidate,i,surface,&present));
                if((qs[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)) != (VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT) || !present) continue;
                float priority=1;
                VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueFamilyIndex=i; qi.queueCount=1; qi.pQueuePriorities=&priority;
                VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; ci.queueCreateInfoCount=1; ci.pQueueCreateInfos=&qi;
                ci.enabledExtensionCount=std::size(extensions); ci.ppEnabledExtensionNames=extensions;
                VkDevice device=wrapper(candidate,opaque,&ci);
                if(!device) continue;
                *context={}; context->gpu=candidate; context->device=device;
                context->queue_family_index=context->presentation_queue_family_index=i;
                vkGetDeviceQueue(device,i,0,&context->queue); context->presentation_queue=context->queue;
                log("Negotiated frontend device through v2 wrapper; graphics/compute/present queue=%u",i);
                frontend_device_negotiated=true;
                return true;
            }
        }
    } catch(const std::exception& e) { log("Negotiation error: %s",e.what()); }
    return false;
}
Bridge::Bridge(const retro_hw_render_interface_vulkan* v, Renderer& r, bool c) : vk(v),dawn(r),cpu(c) {
    require(frontend_device_negotiated,"Frontend fell back from required device negotiation; sharing unavailable");
    require(v && v->interface_type==RETRO_HW_RENDER_INTERFACE_VULKAN && v->interface_version==5,"Vulkan render interface v5 required");
    require(v->get_sync_index && v->get_sync_index_mask && v->wait_sync_index && v->lock_queue && v->unlock_queue && v->set_image,"Incomplete Vulkan v5 callbacks");
    probe_gpu(vk->gpu);
    get_ahb=reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(vk->get_device_proc_addr(vk->device,"vkGetAndroidHardwareBufferPropertiesANDROID"));
    import_fd=reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(vk->get_device_proc_addr(vk->device,"vkImportSemaphoreFdKHR"));
    export_fd=reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vk->get_device_proc_addr(vk->device,"vkGetSemaphoreFdKHR"));
    require(get_ahb && import_fd && export_fd,"Frontend did not enable required extension entry points");
}
Bridge::~Bridge() { try { drain(); } catch(const std::exception& e) { log("Bridge drain error: %s",e.what()); } clear(); }
void Bridge::drain() {
    std::exception_ptr error;
    try { dawn.drain(); } catch(...) { error=std::current_exception(); }
    { QueueLock lock(vk); VK_CHECK(vkQueueWaitIdle(vk->queue)); }
    if(error) std::rethrow_exception(error);
}
void Bridge::destroy(Slot& s) {
    s.texture=nullptr; s.memory=nullptr; s.returned_fd.reset();
    if(s.image.image_view) vkDestroyImageView(vk->device,s.image.image_view,nullptr);
    if(s.shared) vkDestroyImage(vk->device,s.shared,nullptr);
    if(s.output) vkDestroyImage(vk->device,s.output,nullptr);
    if(s.shared_memory) vkFreeMemory(vk->device,s.shared_memory,nullptr);
    if(s.output_memory) vkFreeMemory(vk->device,s.output_memory,nullptr);
    for(auto sem:{s.incoming,s.returned,s.ready}) if(sem) vkDestroySemaphore(vk->device,sem,nullptr);
    if(s.complete) vkDestroyFence(vk->device,s.complete,nullptr);
    if(s.pool) vkDestroyCommandPool(vk->device,s.pool,nullptr);
    if(s.ahb) AHardwareBuffer_release(s.ahb);
}
void Bridge::clear() { for(auto& s:slots) if(s) { destroy(*s); s.reset(); } }
void Bridge::resize(uint32_t w,uint32_t h) {
    uint32_t new_mask=vk->get_sync_index_mask(vk->handle);
    require(new_mask && std::popcount(new_mask)<=8,"Invalid or excessive frontend sync slots (limit 8)");
    if(w==width && h==height && new_mask==mask) return;
    drain(); clear(); width=w; height=h; mask=new_mask;
}
void Bridge::create(Slot& s) {
    AHardwareBuffer_Desc desc{}; desc.width=width; desc.height=height; desc.layers=1;
    desc.format=AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage=AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER|AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    require(AHardwareBuffer_allocate(&desc,&s.ahb)==0,"AHardwareBuffer allocation failed for exact dimensions/usage");
    wgpu::SharedTextureMemoryAHardwareBufferDescriptor ahb{}; ahb.handle=s.ahb;
    wgpu::SharedTextureMemoryDescriptor sd{}; sd.nextInChain=&ahb;
    s.memory=dawn.device.ImportSharedTextureMemory(&sd);
    wgpu::SharedTextureMemoryProperties properties{};
    require(s.memory.GetProperties(&properties)==wgpu::Status::Success,"Dawn AHB import failed");
    require(properties.format==wgpu::TextureFormat::RGBA8Unorm && properties.size.width==width && properties.size.height==height && properties.size.depthOrArrayLayers==1,"Dawn AHB format/size mismatch");
    wgpu::TextureUsage usage=wgpu::TextureUsage::RenderAttachment|wgpu::TextureUsage::CopySrc;
    require((uint64_t(properties.usage) & uint64_t(usage))==uint64_t(usage),"Dawn AHB lacks render/copy usage");
    wgpu::TextureDescriptor td{}; td.size={width,height,1}; td.format=properties.format; td.usage=usage;
    s.texture=s.memory.CreateTexture(&td);
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ci.pNext=&external;
    ci.imageType=VK_IMAGE_TYPE_2D; ci.format=VK_FORMAT_R8G8B8A8_UNORM; ci.extent={width,height,1};
    ci.mipLevels=ci.arrayLayers=1; ci.samples=VK_SAMPLE_COUNT_1_BIT; ci.tiling=VK_IMAGE_TILING_OPTIMAL;
    ci.usage=shared_usage; ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(vk->device,&ci,nullptr,&s.shared));
    VkAndroidHardwareBufferFormatPropertiesANDROID format{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
    VkAndroidHardwareBufferPropertiesANDROID props{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID}; props.pNext=&format;
    VK_CHECK(get_ahb(vk->device,s.ahb,&props));
    require(format.format==VK_FORMAT_R8G8B8A8_UNORM,"AHB Vulkan format mismatch");
    require(format.formatFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT,"AHB missing transfer-source capability");
    // For AHB imports, querying image memory requirements before binding is forbidden.
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO}; dedicated.image=s.shared;
    VkImportAndroidHardwareBufferInfoANDROID import{VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID}; import.buffer=s.ahb; import.pNext=&dedicated;
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; alloc.pNext=&import; alloc.allocationSize=props.allocationSize;
    alloc.memoryTypeIndex=memory_type(vk->gpu,props.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(vk->device,&alloc,nullptr,&s.shared_memory));
    VK_CHECK(vkBindImageMemory(vk->device,s.shared,s.shared_memory,0));
    VkMemoryRequirements shared_requirements{}; vkGetImageMemoryRequirements(vk->device,s.shared,&shared_requirements);
    require(shared_requirements.size<=props.allocationSize &&
        (shared_requirements.memoryTypeBits & (1u<<alloc.memoryTypeIndex)),"Bound AHB memory incompatible with image requirements");
    ci.pNext=nullptr; ci.flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; ci.usage=output_usage;
    VK_CHECK(vkCreateImage(vk->device,&ci,nullptr,&s.output));
    VkMemoryRequirements mr{}; vkGetImageMemoryRequirements(vk->device,s.output,&mr);
    alloc.pNext=nullptr; alloc.allocationSize=mr.size; alloc.memoryTypeIndex=memory_type(vk->gpu,mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(vk->device,&alloc,nullptr,&s.output_memory));
    VK_CHECK(vkBindImageMemory(vk->device,s.output,s.output_memory,0));
    s.image.image_layout=VK_IMAGE_LAYOUT_GENERAL;
    auto& view=s.image.create_info; view.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; view.image=s.output;
    view.viewType=VK_IMAGE_VIEW_TYPE_2D; view.format=VK_FORMAT_R8G8B8A8_UNORM; view.subresourceRange=range();
    VK_CHECK(vkCreateImageView(vk->device,&view,nullptr,&s.image.image_view));
    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VK_CHECK(vkCreateSemaphore(vk->device,&sem,nullptr,&s.incoming));
    VK_CHECK(vkCreateSemaphore(vk->device,&sem,nullptr,&s.ready));
    VkExportSemaphoreCreateInfo export_info{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO}; export_info.handleTypes=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    sem.pNext=&export_info; VK_CHECK(vkCreateSemaphore(vk->device,&sem,nullptr,&s.returned));
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VK_CHECK(vkCreateFence(vk->device,&fence,nullptr,&s.complete));
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pool.queueFamilyIndex=vk->queue_index;
    VK_CHECK(vkCreateCommandPool(vk->device,&pool,nullptr,&s.pool));
    VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; command.commandPool=s.pool; command.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; command.commandBufferCount=1;
    VK_CHECK(vkAllocateCommandBuffers(vk->device,&command,&s.command));
    dawn.events();
}
void Bridge::frame(uint32_t w,uint32_t h) {
    resize(w,h);
    using clock=std::chrono::steady_clock;
    auto ms=[](clock::time_point a,clock::time_point b){ return std::chrono::duration<double,std::milli>(b-a).count(); };
    auto t0=clock::now();
    uint32_t index=vk->get_sync_index(vk->handle);
    require(index<32 && (mask & (1u<<index)),"Frontend sync index outside mask");
    vk->wait_sync_index(vk->handle);
    auto t1=clock::now(); timing.sync+=ms(t0,t1);
    if(!slots[index]) { slots[index]=std::make_unique<Slot>(); create(*slots[index]); }
    Slot& s=*slots[index];
    if(s.submitted) VK_CHECK(vkWaitForFences(vk->device,1,&s.complete,VK_TRUE,UINT64_MAX));
    auto t2=clock::now(); timing.fence+=ms(t1,t2);
    wgpu::SharedFence return_fence;
    uint64_t value=1;
    if(s.returned_fd.value>=0) {
        if(cpu) wait_fd(s.returned_fd.value);
        else {
            wgpu::SharedFenceSyncFDDescriptor fd{}; fd.handle=s.returned_fd.value;
            wgpu::SharedFenceDescriptor fence{}; fence.nextInChain=&fd;
            return_fence=dawn.device.ImportSharedFence(&fence); // Dawn duplicates the borrowed FD.
        }
    }
    wgpu::SharedTextureMemoryVkImageLayoutBeginState begin_layout{}; begin_layout.oldLayout=s.return_old; begin_layout.newLayout=s.return_new;
    wgpu::SharedTextureMemoryBeginAccessDescriptor begin{}; begin.nextInChain=&begin_layout; begin.initialized=s.initialized;
    if(return_fence) { begin.fenceCount=begin.signaledValueCount=1; begin.fences=&return_fence; begin.signaledValues=&value; }
    require(s.memory.BeginAccess(s.texture,&begin)==wgpu::Status::Success,"Dawn BeginAccess failed");
    s.returned_fd.reset();
    try { dawn.render(s.texture); }
    catch(...) {
        // Close the access scope even when validation/encoding fails; teardown then drains.
        wgpu::SharedTextureMemoryVkImageLayoutEndState cleanup_layout{};
        wgpu::SharedTextureMemoryEndAccessState cleanup{}; cleanup.nextInChain=&cleanup_layout;
        s.memory.EndAccess(s.texture,&cleanup);
        throw;
    }
    auto t3=clock::now(); timing.render+=ms(t2,t3);
    wgpu::SharedTextureMemoryVkImageLayoutEndState end_layout{};
    wgpu::SharedTextureMemoryEndAccessState end{}; end.nextInChain=&end_layout;
    require(s.memory.EndAccess(s.texture,&end)==wgpu::Status::Success,"Dawn EndAccess failed");
    require(end.initialized && end.fenceCount==1 && end.signaledValueCount==1 && end.signaledValues[0]==1,"Unexpected Dawn EndAccess fence/initialization state");
    wgpu::SharedFenceSyncFDExportInfo fd_info{};
    wgpu::SharedFenceExportInfo fence_info{}; fence_info.nextInChain=&fd_info;
    end.fences[0].ExportInfo(&fence_info);
    require(fence_info.type==wgpu::SharedFenceType::SyncFD,"Dawn did not export SYNC_FD");
    Fd incoming;
    if(fd_info.handle>=0) { incoming.reset(dup(fd_info.handle)); require(incoming.value>=0,"dup Dawn borrowed SYNC_FD failed"); }
    if(cpu) wait_fd(incoming.value);
    else {
        VkImportSemaphoreFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
        import.semaphore=s.incoming; import.flags=VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
        import.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT; import.fd=incoming.value;
        VK_CHECK(import_fd(vk->device,&import)); incoming.release(); // Vulkan consumes on success (including -1 sentinel).
    }
    VK_CHECK(vkResetFences(vk->device,1,&s.complete)); VK_CHECK(vkResetCommandPool(vk->device,s.pool,0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(s.command,&bi));
    // Ownership acquire must repeat Dawn's exact release pair, then transition separately for the copy.
    auto old=static_cast<VkImageLayout>(end_layout.oldLayout), next=static_cast<VkImageLayout>(end_layout.newLayout);
    barrier(s.command,s.shared,old,next,0,0,VK_QUEUE_FAMILY_FOREIGN_EXT,vk->queue_index);
    barrier(s.command,s.shared,next,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,0,VK_ACCESS_TRANSFER_READ_BIT);
    barrier(s.command,s.output,s.output_initialized?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        s.output_initialized?VK_ACCESS_MEMORY_READ_BIT:0,VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy copy{}; copy.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.dstSubresource=copy.srcSubresource; copy.extent={w,h,1};
    vkCmdCopyImage(s.command,s.shared,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,s.output,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    barrier(s.command,s.output,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
    s.return_old=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; s.return_new=VK_IMAGE_LAYOUT_GENERAL;
    barrier(s.command,s.shared,s.return_old,s.return_new,VK_ACCESS_TRANSFER_READ_BIT,0,vk->queue_index,VK_QUEUE_FAMILY_FOREIGN_EXT);
    VK_CHECK(vkEndCommandBuffer(s.command));
    VkPipelineStageFlags wait_stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSemaphore signals[]={s.returned,s.ready};
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if(!cpu) { submit.waitSemaphoreCount=1; submit.pWaitSemaphores=&s.incoming; submit.pWaitDstStageMask=&wait_stage; }
    submit.commandBufferCount=1; submit.pCommandBuffers=&s.command; submit.signalSemaphoreCount=2; submit.pSignalSemaphores=signals;
    { QueueLock lock(vk); VK_CHECK(vkQueueSubmit(vk->queue,1,&submit,s.complete)); }
    s.submitted=true; s.output_initialized=true; s.initialized=end.initialized;
    VkSemaphoreGetFdInfoKHR export_info{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
    export_info.semaphore=s.returned; export_info.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    int return_fd=-1; VK_CHECK(export_fd(vk->device,&export_info,&return_fd)); s.returned_fd.reset(return_fd);
    timing.submit+=ms(t3,clock::now());
    vk->set_image(vk->handle,&s.image,1,&s.ready,vk->queue_index);
}
}
