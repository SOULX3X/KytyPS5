#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/vulkanInstance.h"

#include <memory>
#include <mutex>
#include <vector>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan_core.h> // IWYU pragma: export

namespace Libs::Graphics {

class CommandBuffer;

struct VulkanSwapchain {
	~VulkanSwapchain();

	VkSwapchainKHR                 swapchain        = nullptr;
	VkFormat                       swapchain_format = VK_FORMAT_UNDEFINED;
	VkExtent2D                     swapchain_extent = {};
	std::unique_ptr<VkImage[]>     swapchain_images;
	std::unique_ptr<VkImageView[]> swapchain_image_views;
	uint32_t                       swapchain_images_count = 0;
	std::unique_ptr<VkSemaphore[]> image_acquired_semaphores;
	std::unique_ptr<VkSemaphore[]> render_complete_semaphores;
	uint32_t                       current_index = 0;
	uint32_t                       present_frame = 0;
};

struct VulkanCommandPool {
	Common::Mutex                      mutex;
	VkCommandPool                      pool = nullptr;
	std::unique_ptr<VkCommandBuffer[]> buffers;
	std::unique_ptr<VkFence[]>         fences;
	std::unique_ptr<VkSemaphore[]>     semaphores;
	std::unique_ptr<bool[]>            busy;
	uint32_t                           buffers_count = 0;
};

struct GraphicContext: public VulkanInstance {
	uint32_t screen_width  = 0;
	uint32_t screen_height = 0;
};

struct VulkanMemory {
	VkMemoryRequirements  requirements    = {};
	VkMemoryPropertyFlags property        = 0;
	VkDeviceMemory        memory          = nullptr;
	VmaAllocation         allocation      = nullptr;
	VmaAllocationInfo     allocation_info = {};
	VkDeviceSize          offset          = 0;
	uint32_t              type            = 0;
	uint64_t              unique_id       = 0;
};

enum class VulkanImageType {
	Unknown,
	VideoOut,
	DepthStencil,
	Texture,
	StorageTexture,
	RenderTexture
};

struct ImageViewInfo {
	VkFormat           format      = VK_FORMAT_UNDEFINED;
	VkImageViewType    type        = VK_IMAGE_VIEW_TYPE_2D;
	VkImageAspectFlags aspect      = 0;
	uint32_t           base_level  = 0;
	uint32_t           level_count = 0;
	uint32_t           base_layer  = 0;
	uint32_t           layer_count = 1;
	uint32_t           swizzle     = 0;
	VkImageUsageFlags  usage       = VK_IMAGE_USAGE_SAMPLED_BIT;

	bool operator==(const ImageViewInfo&) const = default;
};

struct CachedImageView {
	ImageViewInfo info;
	VkImageView   view = nullptr;
};

struct ImageViewCache {
	std::mutex                   mutex;
	std::vector<CachedImageView> views;

	ImageViewCache() = default;
	KYTY_CLASS_NO_COPY(ImageViewCache);
};

struct VulkanImage {
	static constexpr int VIEW_MAX           = 4;
	static constexpr int VIEW_DEFAULT       = 0;
	static constexpr int VIEW_DEFAULT_ARRAY = 1;
	static constexpr int VIEW_STORAGE       = 2;
	static constexpr int VIEW_STORAGE_ARRAY = 3;

	explicit VulkanImage(VulkanImageType type): type(type) {}
	KYTY_CLASS_NO_COPY(VulkanImage);

	VulkanImageType        type                 = VulkanImageType::Unknown;
	VkFormat               format               = VK_FORMAT_UNDEFINED;
	VkExtent2D             extent               = {};
	uint32_t               guest_pitch          = 0;
	uint32_t               layers               = 1;
	uint32_t               mip_levels           = 1;
	VkImage                image                = nullptr;
	VkImageView            image_view[VIEW_MAX] = {};
	VkImageLayout          layout               = VK_IMAGE_LAYOUT_UNDEFINED;
	Graphics::VulkanMemory memory;
	ImageViewCache         view_cache;
};

struct VideoOutVulkanImage: public VulkanImage {
	VideoOutVulkanImage(): VulkanImage(VulkanImageType::VideoOut) {}
};

struct DepthStencilVulkanImage: public VulkanImage {
	DepthStencilVulkanImage(): VulkanImage(VulkanImageType::DepthStencil) {}
	bool compressed = false;
};

struct GpuTextureVulkanImage: public VulkanImage {
	explicit GpuTextureVulkanImage(VulkanImageType type): VulkanImage(type) {}
};

struct TextureVulkanImage: public GpuTextureVulkanImage {
	TextureVulkanImage(): GpuTextureVulkanImage(VulkanImageType::Texture) {}
};

struct StorageTextureVulkanImage: public GpuTextureVulkanImage {
	StorageTextureVulkanImage(): GpuTextureVulkanImage(VulkanImageType::StorageTexture) {}
};

struct RenderTextureVulkanImage: public VulkanImage {
	RenderTextureVulkanImage(): VulkanImage(VulkanImageType::RenderTexture) {}
};

struct VulkanBuffer {
	VkBuffer           buffer = nullptr;
	VulkanMemory       memory;
	VkBufferUsageFlags usage       = 0;
	uint64_t           buffer_size = 0;
};

struct StorageVulkanBuffer: public VulkanBuffer {};

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_ */
