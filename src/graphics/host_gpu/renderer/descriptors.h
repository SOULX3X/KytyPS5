#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_

#include "graphics/shader/recompiler/ShaderIR.h"
#include "graphics/shader/shaderBindings.h"

#include <cstdint>
#include <vulkan/vulkan_core.h>

namespace Libs::Graphics {

struct VulkanImage;

struct TargetTextureViewInfo {
	VkImageViewType type        = VK_IMAGE_VIEW_TYPE_MAX_ENUM;
	uint32_t        base_layer  = 0;
	uint32_t        layer_count = 0;
};

[[nodiscard]] TargetTextureViewInfo
ResolveTargetTextureView(const ShaderRecompiler::IR::ImageResource& resource,
                         Prospero::ImageType type, uint32_t base_layer, uint32_t image_layers);

[[nodiscard]] bool IsSupportedDepthTargetDescriptor(const ShaderTextureResource& descriptor,
                                                    const VulkanImage&           image);
[[nodiscard]] bool
IsSupportedSampledVideoOutView(const ShaderRecompiler::IR::ImageResource& resource,
                               const ShaderTextureResource& descriptor, const VulkanImage& image);
void ValidateMetadataReuseTexture(const ShaderRecompiler::IR::ImageResource& resource,
                                  const ShaderTextureResource& descriptor, uint64_t size);
void ValidateStorageTexture(const ShaderRecompiler::IR::ImageResource& resource,
                            const ShaderTextureResource& descriptor, uint64_t size);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_
