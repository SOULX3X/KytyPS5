#include "graphics/host_gpu/renderer/imageView.h"

#include "common/assert.h"
#include "graphics/host_gpu/objects/textureCommon.h"
#include "graphics/host_gpu/renderer/textureCache.h"
#include "graphics/host_gpu/utils.h"

#include <mutex>

namespace Libs::Graphics {

namespace {

void CreateRenderTargetView(GraphicContext* ctx, VulkanImage* image, int index,
                            VkComponentSwizzle r, VkComponentSwizzle g, VkComponentSwizzle b,
                            VkComponentSwizzle a, VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D,
                            VkFormat          view_format = VK_FORMAT_UNDEFINED,
                            VkImageUsageFlags view_usage = 0, uint32_t level_count = 0) {
	const auto layer_count = type == VK_IMAGE_VIEW_TYPE_2D_ARRAY ? image->layers : 1u;
	UtilCreateImageView(ctx, image, index, type, VK_IMAGE_ASPECT_COLOR_BIT, {r, g, b, a}, 0, 0,
	                    layer_count, level_count == 0 ? image->mip_levels : level_count,
	                    view_format, view_usage);
}

} // namespace

namespace ImageViewOps {

VkImageAspectFlags DepthAspectMask(VkFormat format) noexcept {
	return VK_IMAGE_ASPECT_DEPTH_BIT |
	       (format == VK_FORMAT_D16_UNORM_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT ||
	                format == VK_FORMAT_D32_SFLOAT_S8_UINT
	            ? VK_IMAGE_ASPECT_STENCIL_BIT
	            : 0u);
}

bool FormatSupportsStorage(GraphicContext* ctx, VkFormat format) {
	const auto properties = ctx->GetFormatProperties(format);
	return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
}

void CreateRenderTargetViews(GraphicContext* ctx, RenderTextureVulkanImage* image) {
	CreateRenderTargetView(ctx, image, VulkanImage::VIEW_DEFAULT, VK_COMPONENT_SWIZZLE_IDENTITY,
	                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
	                       VK_COMPONENT_SWIZZLE_IDENTITY);
	if (image->layers > 1) {
		CreateRenderTargetView(ctx, image, VulkanImage::VIEW_DEFAULT_ARRAY,
		                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
		                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
		                       VK_IMAGE_VIEW_TYPE_2D_ARRAY);
	}
	if (FormatSupportsStorage(ctx, image->format)) {
		CreateRenderTargetView(ctx, image, VulkanImage::VIEW_STORAGE, VK_COMPONENT_SWIZZLE_IDENTITY,
		                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
		                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_IMAGE_VIEW_TYPE_2D,
		                       VK_FORMAT_UNDEFINED, 0, 1);
		if (image->layers > 1) {
			CreateRenderTargetView(ctx, image, VulkanImage::VIEW_STORAGE_ARRAY,
			                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
			                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
			                       VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_FORMAT_UNDEFINED, 0, 1);
		}
	}
}

void CreateDepthViews(GraphicContext* ctx, DepthStencilVulkanImage* image) {
	UtilCreateImageView(ctx, image, VulkanImage::VIEW_DEFAULT, VK_IMAGE_VIEW_TYPE_2D,
	                    DepthAspectMask(image->format),
	                    {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
	                     VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
	                    0, 0, 1, 1);
}

void CreateVideoOutViews(GraphicContext* ctx, VideoOutVulkanImage* image) {
	CreateRenderTargetView(ctx, image, VulkanImage::VIEW_DEFAULT, VK_COMPONENT_SWIZZLE_IDENTITY,
	                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
	                       VK_COMPONENT_SWIZZLE_IDENTITY);
	if ((image->format == VK_FORMAT_R8G8B8A8_SRGB || image->format == VK_FORMAT_B8G8R8A8_SRGB) &&
	    FormatSupportsStorage(ctx, VK_FORMAT_R8G8B8A8_UINT)) {
		CreateRenderTargetView(ctx, image, VulkanImage::VIEW_STORAGE, VK_COMPONENT_SWIZZLE_IDENTITY,
		                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
		                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_IMAGE_VIEW_TYPE_2D,
		                       VK_FORMAT_R8G8B8A8_UINT, VK_IMAGE_USAGE_STORAGE_BIT, 1);
	}
}

void DestroyViews(GraphicContext* ctx, VulkanImage* image) {
	EXIT_IF(ctx == nullptr);
	EXIT_IF(image == nullptr);

	for (auto& cached: image->view_cache.views) {
		if (cached.view != nullptr) {
			vkDestroyImageView(ctx->device, cached.view, nullptr);
			cached.view = nullptr;
		}
	}
	image->view_cache.views.clear();
	for (auto& view: image->image_view) {
		if (view != nullptr) {
			vkDestroyImageView(ctx->device, view, nullptr);
			view = nullptr;
		}
	}
}

} // namespace ImageViewOps

VkImageView TextureCache::GetRenderTargetAttachmentView(GraphicContext*           ctx,
                                                        RenderTextureVulkanImage* image,
                                                        VkFormat format, uint32_t level,
                                                        uint32_t base_layer, uint32_t layer_count) {
	if (ctx == nullptr || image == nullptr || image->image == nullptr ||
	    format == VK_FORMAT_UNDEFINED || level >= image->mip_levels || level >= 16 ||
	    layer_count == 0 || base_layer >= image->layers ||
	    layer_count > image->layers - base_layer) {
		EXIT("TextureCache: invalid render-target attachment view, image=%p format=%d"
		     " level=%u image_levels=%u base_layer=%u layer_count=%u image_layers=%u\n",
		     static_cast<const void*>(image), static_cast<int>(format), level,
		     image != nullptr ? image->mip_levels : 0, base_layer, layer_count,
		     image != nullptr ? image->layers : 0);
	}
	if (format != image->format && !IsRgba8SrgbReinterpretation(image->format, format)) {
		EXIT("TextureCache: incompatible render-target attachment view, image_format=%d"
		     " view_format=%d level=%u\n",
		     static_cast<int>(image->format), static_cast<int>(format), level);
	}

	return GetImageView(ctx, image,
	                    {format,
	                     layer_count == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY,
	                     VK_IMAGE_ASPECT_COLOR_BIT, level, 1, base_layer, layer_count,
	                     DstSel(4, 5, 6, 7), VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT});
}

VkImageView TextureCache::GetDepthTargetAttachmentView(GraphicContext*          ctx,
                                                       DepthStencilVulkanImage* image,
                                                       uint32_t base_layer, uint32_t layer_count) {
	if (ctx == nullptr || image == nullptr || image->image == nullptr || layer_count == 0 ||
	    base_layer >= image->layers || layer_count > image->layers - base_layer) {
		EXIT("TextureCache: invalid depth-target attachment view, image=%p base_layer=%u "
		     "layer_count=%u image_layers=%u\n",
		     static_cast<const void*>(image), base_layer, layer_count,
		     image != nullptr ? image->layers : 0);
	}
	return GetImageView(
	    ctx, image,
	    {image->format, layer_count == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY,
	     ImageViewOps::DepthAspectMask(image->format), 0, 1, base_layer, layer_count,
	     DstSel(4, 5, 6, 7), VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT});
}

VkImageView TextureCache::GetImageView(GraphicContext* ctx, VulkanImage* image,
                                       const ImageViewInfo& info) {
	const bool supported_type  = info.type == VK_IMAGE_VIEW_TYPE_2D ||
	                             info.type == VK_IMAGE_VIEW_TYPE_2D_ARRAY ||
	                             info.type == VK_IMAGE_VIEW_TYPE_3D;
	const bool supported_usage = info.usage == VK_IMAGE_USAGE_SAMPLED_BIT ||
	                             info.usage == VK_IMAGE_USAGE_STORAGE_BIT ||
	                             info.usage == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT ||
	                             info.usage == VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	const bool valid_shape =
	    (info.type == VK_IMAGE_VIEW_TYPE_2D && info.layer_count == 1) ||
	    info.type == VK_IMAGE_VIEW_TYPE_2D_ARRAY ||
	    (info.type == VK_IMAGE_VIEW_TYPE_3D && info.base_layer == 0 && info.layer_count == 1);
	if (ctx == nullptr || image == nullptr || image->image == nullptr ||
	    info.format == VK_FORMAT_UNDEFINED || info.aspect == 0 || info.level_count == 0 ||
	    info.base_level >= (image != nullptr ? image->mip_levels : 0) ||
	    info.level_count > image->mip_levels - info.base_level || info.layer_count == 0 ||
	    info.base_layer >= image->layers || info.layer_count > image->layers - info.base_layer ||
	    !supported_type || !valid_shape || !supported_usage) {
		EXIT("TextureCache: invalid dynamic image view, image=%p format=%d aspect=0x%x"
		     " swizzle=0x%03x mip=%u+%u layer=%u+%u type=%d usage=0x%x"
		     " image_levels=%u image_layers=%u\n",
		     static_cast<const void*>(image), static_cast<int>(info.format), info.aspect,
		     info.swizzle, info.base_level, info.level_count, info.base_layer, info.layer_count,
		     static_cast<int>(info.type), info.usage, image != nullptr ? image->mip_levels : 0,
		     image != nullptr ? image->layers : 0);
	}

	auto&           cache = image->view_cache;
	std::lock_guard lock(cache.mutex);
	for (const auto& cached: cache.views) {
		if (cached.info == info) {
			return cached.view;
		}
	}

	VkImageViewUsageCreateInfo usage {};
	usage.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
	usage.usage = info.usage;
	VkImageViewCreateInfo create {};
	create.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	create.pNext                           = &usage;
	create.image                           = image->image;
	create.viewType                        = info.type;
	create.format                          = info.format;
	create.components                      = info.usage == VK_IMAGE_USAGE_SAMPLED_BIT
	                                             ? TextureGetComponentMapping(info.swizzle)
	                                             : VkComponentMapping {};
	create.subresourceRange.aspectMask     = info.aspect;
	create.subresourceRange.baseMipLevel   = info.base_level;
	create.subresourceRange.levelCount     = info.level_count;
	create.subresourceRange.baseArrayLayer = info.base_layer;
	create.subresourceRange.layerCount     = info.layer_count;
	VkImageView view                       = nullptr;
	const auto  result = vkCreateImageView(ctx->device, &create, nullptr, &view);
	if (result != VK_SUCCESS || view == nullptr) {
		EXIT("TextureCache: failed to create dynamic image view, result=%d format=%d"
		     " aspect=0x%x swizzle=0x%03x mip=%u+%u layer=%u+%u type=%d usage=0x%x\n",
		     static_cast<int>(result), static_cast<int>(info.format), info.aspect, info.swizzle,
		     info.base_level, info.level_count, info.base_layer, info.layer_count,
		     static_cast<int>(info.type), info.usage);
	}
	cache.views.push_back({info, view});
	return view;
}

VkImageView TextureCache::GetDepthTargetSampledView(GraphicContext*          ctx,
                                                    DepthStencilVulkanImage* image,
                                                    VkFormat view_format, uint32_t swizzle,
                                                    uint32_t base_level, uint32_t level_count,
                                                    VkImageViewType type, uint32_t base_layer,
                                                    uint32_t layer_count) {
	if (ctx == nullptr || image == nullptr || image->image == nullptr ||
	    view_format == VK_FORMAT_UNDEFINED ||
	    !IsSupportedSampledDepthView(image->format, view_format, swizzle)) {
		EXIT("TextureCache: invalid sampled depth-target view, image=%p image_format=%d"
		     " view_format=%d swizzle=0x%03x mip=%u+%u layer=%u+%u type=%d"
		     " image_levels=%u image_layers=%u\n",
		     static_cast<const void*>(image),
		     image != nullptr ? static_cast<int>(image->format) : VK_FORMAT_UNDEFINED,
		     static_cast<int>(view_format), swizzle, base_level, level_count, base_layer,
		     layer_count, static_cast<int>(type), image != nullptr ? image->mip_levels : 0,
		     image != nullptr ? image->layers : 0);
	}
	return GetImageView(ctx, image,
	                    {image->format, type, VK_IMAGE_ASPECT_DEPTH_BIT, base_level, level_count,
	                     base_layer, layer_count, swizzle});
}

VkImageView TextureCache::GetSampledColorView(GraphicContext* ctx, VulkanImage* image,
                                              VkFormat view_format, uint32_t swizzle,
                                              uint32_t base_level, uint32_t level_count,
                                              VkImageViewType type, uint32_t base_layer,
                                              uint32_t layer_count) {
	if (ctx == nullptr || image == nullptr || image->image == nullptr ||
	    view_format == VK_FORMAT_UNDEFINED || base_level >= 16 ||
	    (type != VK_IMAGE_VIEW_TYPE_2D && type != VK_IMAGE_VIEW_TYPE_2D_ARRAY) ||
	    !IsSupportedSampledColorView(image->format, view_format, swizzle)) {
		EXIT("TextureCache: invalid sampled color view, image=%p swizzle=0x%03x"
		     " view_format=%d mip=%u+%u layer=%u+%u type=%d image_levels=%u image_layers=%u\n",
		     static_cast<const void*>(image), swizzle, static_cast<int>(view_format), base_level,
		     level_count, base_layer, layer_count, static_cast<int>(type),
		     image != nullptr ? image->mip_levels : 0, image != nullptr ? image->layers : 0);
	}
	const auto precreated_view = type == VK_IMAGE_VIEW_TYPE_2D_ARRAY
	                                 ? VulkanImage::VIEW_DEFAULT_ARRAY
	                                 : VulkanImage::VIEW_DEFAULT;
	const bool full_view =
	    base_level == 0 && level_count == image->mip_levels && base_layer == 0 &&
	    layer_count == (type == VK_IMAGE_VIEW_TYPE_2D_ARRAY ? image->layers : 1u);
	if (view_format == image->format && swizzle == DstSel(4, 5, 6, 7) && full_view &&
	    image->image_view[precreated_view] != nullptr) {
		return image->image_view[precreated_view];
	}
	return GetImageView(ctx, image,
	                    {view_format, type, VK_IMAGE_ASPECT_COLOR_BIT, base_level, level_count,
	                     base_layer, layer_count, swizzle});
}

VkImageView TextureCache::GetRenderTargetStorageView(GraphicContext*           ctx,
                                                     RenderTextureVulkanImage* image,
                                                     VkFormat view_format, uint32_t base_level,
                                                     uint32_t level_count, VkImageViewType type,
                                                     uint32_t base_layer, uint32_t layer_count) {
	if (ctx == nullptr || image == nullptr || image->image == nullptr ||
	    view_format == VK_FORMAT_UNDEFINED ||
	    (type != VK_IMAGE_VIEW_TYPE_2D && type != VK_IMAGE_VIEW_TYPE_2D_ARRAY)) {
		EXIT("TextureCache: invalid render-target storage view, image=%p view_format=%d"
		     " mip=%u+%u layer=%u+%u type=%d image_levels=%u image_layers=%u\n",
		     static_cast<const void*>(image), static_cast<int>(view_format), base_level,
		     level_count, base_layer, layer_count, static_cast<int>(type),
		     image != nullptr ? image->mip_levels : 0, image != nullptr ? image->layers : 0);
	}
	const bool exact      = view_format == image->format;
	const bool compatible = view_format == BgraSrgbStorageViewFormat(image->format);
	if (!exact && !compatible) {
		EXIT("TextureCache: incompatible render-target storage view, image_format=%d"
		     " view_format=%d base=%u count=%u\n",
		     static_cast<int>(image->format), static_cast<int>(view_format), base_level,
		     level_count);
	}
	if (exact) {
		const auto index = type == VK_IMAGE_VIEW_TYPE_2D_ARRAY ? VulkanImage::VIEW_STORAGE_ARRAY
		                                                       : VulkanImage::VIEW_STORAGE;
		const bool full_view =
		    base_level == 0 && level_count == 1 && base_layer == 0 &&
		    layer_count == (type == VK_IMAGE_VIEW_TYPE_2D_ARRAY ? image->layers : 1u);
		if (full_view && image->image_view[index] != nullptr) {
			return image->image_view[index];
		}
	}

	if (compatible && !ImageViewOps::FormatSupportsStorage(ctx, view_format)) {
		EXIT("TextureCache: compatible render-target storage format lacks storage support,"
		     " image_format=%d view_format=%d base=%u count=%u\n",
		     static_cast<int>(image->format), static_cast<int>(view_format), base_level,
		     level_count);
	}
	return GetImageView(ctx, image,
	                    {view_format, type, VK_IMAGE_ASPECT_COLOR_BIT, base_level, level_count,
	                     base_layer, layer_count, DstSel(4, 5, 6, 7), VK_IMAGE_USAGE_STORAGE_BIT});
}

VkImageView TextureCache::GetStorageTextureSampledView(GraphicContext*            ctx,
                                                       StorageTextureVulkanImage* image,
                                                       const ImageInfo&           info) {
	const auto shape =
	    SelectStorageSampledViewShape(info.type, info.depth, image != nullptr ? image->layers : 0);
	if (ctx == nullptr || image == nullptr || image->image == nullptr ||
	    shape == StorageSampledViewShape::Unsupported || info.base_array != 0 ||
	    info.levels != image->mip_levels || info.base_level >= info.levels ||
	    info.view_levels == 0 || info.base_level + info.view_levels > info.levels) {
		EXIT("TextureCache: invalid sampled view of storage texture, image=%p type=%u depth=%u"
		     " base=%u levels=%u view_levels=%u image_levels=%u base_array=%u\n",
		     static_cast<const void*>(image), info.type, info.depth, info.base_level, info.levels,
		     info.view_levels, image != nullptr ? image->mip_levels : 0, info.base_array);
	}
	const auto view_format = TextureGetFormat(info.format, TextureFormatUsage::Sampled);
	if (view_format != image->format && !IsRgba8SrgbReinterpretation(image->format, view_format) &&
	    !IsR32UintFloatReinterpretation(image->format, view_format)) {
		EXIT("TextureCache: incompatible sampled view of storage texture, image_format=%d"
		     " view_format=%d swizzle=0x%03x\n",
		     static_cast<int>(image->format), static_cast<int>(view_format), info.swizzle);
	}

	VkImageViewType type = VK_IMAGE_VIEW_TYPE_MAX_ENUM;
	switch (shape) {
		case StorageSampledViewShape::Image2D: type = VK_IMAGE_VIEW_TYPE_2D; break;
		case StorageSampledViewShape::Image2DArray: type = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
		case StorageSampledViewShape::Image3D: type = VK_IMAGE_VIEW_TYPE_3D; break;
		case StorageSampledViewShape::Unsupported:
			EXIT("TextureCache: unsupported sampled storage-image view shape\n");
	}
	const auto layer_count = shape == StorageSampledViewShape::Image2DArray ? info.depth : 1u;
	return GetImageView(ctx, image,
	                    {view_format, type, VK_IMAGE_ASPECT_COLOR_BIT, info.base_level,
	                     info.view_levels, 0, layer_count, info.swizzle});
}

VkImageView TextureCache::GetStorageTextureStorageView(GraphicContext*            ctx,
                                                       StorageTextureVulkanImage* image,
                                                       uint32_t                   base_level) {
	if (ctx == nullptr || image == nullptr || image->image == nullptr ||
	    base_level >= (image != nullptr ? image->mip_levels : 0)) {
		EXIT("TextureCache: invalid storage-texture mip view, image=%p level=%u levels=%u\n",
		     static_cast<const void*>(image), base_level, image != nullptr ? image->mip_levels : 0);
	}
	if (base_level == 0) {
		return image->image_view[VulkanImage::VIEW_DEFAULT];
	}
	return GetImageView(ctx, image,
	                    {image->format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT,
	                     base_level, 1, 0, 1, DstSel(4, 5, 6, 7), VK_IMAGE_USAGE_STORAGE_BIT});
}

} // namespace Libs::Graphics
