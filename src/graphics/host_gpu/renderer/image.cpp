#include "graphics/host_gpu/renderer/image.h"

#include "common/assert.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/objects/textureCommon.h"
#include "graphics/host_gpu/regionDefinitions.h"
#include "graphics/host_gpu/renderer/framebufferCache.h"
#include "graphics/host_gpu/renderer/imageView.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/utils.h"
#include "graphics/shader/shader.h"

#include <algorithm>

namespace Libs::Graphics {

namespace {

TextureImageCreateParams MakeImageParams(const ImageInfo& info, bool storage) {
	TextureImageCreateParams params {};
	params.fmt        = info.format;
	params.width      = info.width;
	params.height     = info.height;
	params.base_level = SelectImageBackingBaseLevel(storage, info.base_level);
	params.levels     = info.levels;
	params.depth      = info.depth;
	params.type       = info.type;
	// Storage image views use identity component mapping. The guest storage write mapping is
	// validated before this point and intentionally does not become a Vulkan view swizzle.
	params.swizzle               = storage ? DstSel(4, 5, 6, 7) : info.swizzle;
	params.format_usage          = TextureFormatUsage::Sampled | TextureFormatUsage::Storage;
	params.required_format_usage = storage
	                                   ? TextureFormatUsage::Sampled | TextureFormatUsage::Storage
	                                   : TextureFormatUsage::Sampled;
	params.view_usage      = storage ? TextureFormatUsage::Sampled | TextureFormatUsage::Storage
	                                 : TextureFormatUsage::Sampled;
	params.image_layout    = TextureUploadDestination::MipLevels;
	params.allow_cube_view = !storage;
	params.compatible_format_views =
	    storage && (IsRgba8SrgbViewFormat(TextureGetFormat(info.format, params.format_usage)) ||
	                info.format == Prospero::GpuEnumValue(Prospero::BufferFormat::k32UInt) ||
	                info.format == Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float));
	params.owner = storage ? "StorageTextureCache" : "TextureCache";
	return params;
}

bool RenderTargetSupportsStorage(GraphicContext* ctx, VkFormat format, VkImageCreateFlags flags) {
	const auto compatible = SrgbStorageViewFormat(format);
	const auto required_flags =
	    VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
	const bool compatible_views = (flags & required_flags) == required_flags;
	return ImageViewOps::FormatSupportsStorage(ctx, format) ||
	       (compatible_views && compatible != VK_FORMAT_UNDEFINED &&
	        ImageViewOps::FormatSupportsStorage(ctx, compatible));
}

VkImageCreateFlags RenderTargetCreateFlags(VkFormat format) {
	const bool compatible_format_view =
	    IsRgba8SrgbViewFormat(format) ||
	    BgraToRgbaSampledViewFormat(format) != VK_FORMAT_UNDEFINED ||
	    format == VK_FORMAT_R8G8B8A8_UINT || format == VK_FORMAT_R16G16B16A16_SFLOAT ||
	    format == VK_FORMAT_R16G16B16A16_UINT;
	return compatible_format_view
	           ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT
	           : VkImageCreateFlags {0};
}

VkImageUsageFlags RenderTargetUsage(GraphicContext* ctx, VkFormat format,
                                    VkImageCreateFlags flags) {
	auto usage = static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
	             static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSFER_SRC_BIT) |
	             static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSFER_DST_BIT) |
	             static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_SAMPLED_BIT);
	if (RenderTargetSupportsStorage(ctx, format, flags)) {
		usage |= VK_IMAGE_USAGE_STORAGE_BIT;
	}
	VkImageFormatProperties properties {};
	if (ctx->GetImageFormatProperties(format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage,
	                                  flags, &properties) != VK_SUCCESS) {
		EXIT("TextureCache: render-target format does not support required usage, format=%d "
		     "usage=0x%x\n",
		     static_cast<int>(format), usage);
	}
	return usage;
}

[[nodiscard]] uint32_t RenderTargetTransferFormatImpl(uint32_t bytes_per_element) {
	switch (bytes_per_element) {
		case 1: return Prospero::GpuEnumValue(Prospero::BufferFormat::k8UNorm);
		case 2: return Prospero::GpuEnumValue(Prospero::BufferFormat::k16UNorm);
		case 4: return Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float);
		case 8: return Prospero::GpuEnumValue(Prospero::BufferFormat::k16_16_16_16Float);
		case 16: return Prospero::GpuEnumValue(Prospero::BufferFormat::k32_32_32_32Float);
		default:
			EXIT("TextureCache: unsupported render-target element size: %u\n", bytes_per_element);
	}
}

static constexpr uint32_t DummyTextureSwizzle() {
	return Prospero::GpuEnumValue(Prospero::CompSwizzle::kRed) |
	       (Prospero::GpuEnumValue(Prospero::CompSwizzle::kGreen) << 3u) |
	       (Prospero::GpuEnumValue(Prospero::CompSwizzle::kBlue) << 6u) |
	       (Prospero::GpuEnumValue(Prospero::CompSwizzle::kAlpha) << 9u);
}

TextureImageCreateParams MakeDummyTextureParams(bool uint_format, bool image_3d,
                                                TextureFormatUsage usage, const char* owner) {
	TextureImageCreateParams params {};
	params.fmt = static_cast<uint32_t>(
	    Prospero::GpuEnumValue(uint_format ? Prospero::BufferFormat::k8_8_8_8UInt
	                                       : Prospero::BufferFormat::k8_8_8_8UNorm));
	params.width                 = 1;
	params.height                = 1;
	params.base_level            = 0;
	params.levels                = 1;
	params.depth                 = 1;
	params.type                  = Prospero::GpuEnumValue(image_3d ? Prospero::ImageType::kColor3D
	                                                               : Prospero::ImageType::kColor2D);
	params.swizzle               = DummyTextureSwizzle();
	params.format_usage          = usage;
	params.required_format_usage = usage;
	params.view_usage            = usage;
	params.image_layout          = TextureUploadDestination::MipLevels;
	params.allow_cube_view       = true;
	params.storage_swizzle_fallback = TextureHasFormatUsage(usage, TextureFormatUsage::Storage);
	params.owner                    = owner;
	return params;
}

} // namespace

namespace ImageOps {

uint32_t RenderTargetTransferFormat(uint32_t bytes_per_element) {
	return RenderTargetTransferFormatImpl(bytes_per_element);
}

GpuTextureVulkanImage* CreateTexture(GraphicContext* ctx, const ImageInfo& info, bool storage,
                                     VulkanMemory* memory, VkComponentMapping* components) {
	if (components == nullptr) {
		EXIT("TextureCache: invalid texture component output\n");
	}
	auto* image = storage ? static_cast<GpuTextureVulkanImage*>(new StorageTextureVulkanImage)
	                      : new TextureVulkanImage;
	*components = TextureCreateImage(ctx, image, memory, MakeImageParams(info, storage));
	return image;
}

void CreateTextureViews(GraphicContext* ctx, GpuTextureVulkanImage* image, const ImageInfo& info,
                        bool storage, VkComponentMapping components) {
	if (storage) {
		TextureCreateImageViews(ctx, image, components, info.type, 0, 0, 1, info.depth, false,
		                        TextureFormatUsage::Sampled | TextureFormatUsage::Storage);
	} else {
		TextureCreateImageViews(ctx, image, components, info.type, info.base_array, info.base_level,
		                        info.view_levels, info.depth, true, TextureFormatUsage::Sampled);
	}
}

void UploadRenderTargetLayers(GraphicContext* ctx, RenderTextureVulkanImage* image,
                              const RenderTargetInfo& info, uint32_t base_layer,
                              uint32_t layer_count, bool refresh) {
	if (info.layers == 0 || info.size % info.layers != 0 || layer_count == 0 ||
	    base_layer >= info.layers || layer_count > info.layers - base_layer || image == nullptr ||
	    base_layer >= image->layers || layer_count > image->layers - base_layer) {
		EXIT("TextureCache: invalid render-target layer upload, base=%u count=%u "
		     "info_layers=%u image_layers=%u size=0x%016" PRIx64 "\n",
		     base_layer, layer_count, info.layers, image != nullptr ? image->layers : 0, info.size);
	}
	if (refresh) {
		VulkanDeviceWaitIdle(ctx);
	}
	const auto slice_size  = info.size / info.layers;
	const auto upload_size = slice_size * layer_count;
	const bool standard64  = IsSupportedStandard64RenderTarget(info);
	if (standard64 || info.levels > 1 || info.layers > 1) {
		const auto format = RenderTargetTransferFormat(info.bytes_per_element);
		auto layout = TextureCalcUploadLayout(format, info.width, info.height, info.levels,
		                                      layer_count, info.pitch, info.tile_mode, upload_size,
		                                      false, false, false, "TextureCache render target");
		const bool render_target_tiled =
		    info.tile_mode == Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget);
		if (!standard64 && ((render_target_tiled && !layout.fmt_tiled_render_target) ||
		                    layout.pitch != info.pitch)) {
			EXIT("TextureCache: unsupported render-target mip upload layout, pitch=%u/%u tile=%u\n",
			     info.pitch, layout.pitch, info.tile_mode);
		}
		auto regions = TextureBuildUploadRegions(
		    layout, info.format, info.width, info.height, layer_count, info.levels, true, false,
		    TextureUploadDestination::MipLevels, TextureUploadSliceLayout::MipChainPerSlice);
		for (auto& region: regions) {
			region.dst_layer += base_layer;
		}
		const auto source_address = info.address + slice_size * base_layer;
		TextureUploadGuestImage(
		    ctx, image, reinterpret_cast<const void*>(source_address), upload_size, regions, layout,
		    format, info.width, info.height, layer_count, info.levels,
		    TextureUploadSliceLayout::MipChainPerSlice, "TextureCache render target",
		    static_cast<uint64_t>(VK_IMAGE_LAYOUT_GENERAL));
		return;
	}
	if (info.tile_mode == Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget) &&
	    UtilBufferIsTiled(info.address, slice_size)) {
		UtilScratchBuffer scratch(slice_size);
		TileConvertTiledToLinearRenderTarget(
		    scratch.Data(), reinterpret_cast<const void*>(info.address), info.width, info.height,
		    info.pitch, info.bytes_per_element, slice_size);
		UtilFillImage(ctx, image, scratch.Data(), slice_size, info.pitch,
		              static_cast<uint64_t>(VK_IMAGE_LAYOUT_GENERAL));
	} else {
		UtilFillImage(ctx, image, reinterpret_cast<const void*>(info.address), slice_size,
		              info.pitch, static_cast<uint64_t>(VK_IMAGE_LAYOUT_GENERAL));
	}
}

void UploadRenderTarget(GraphicContext* ctx, RenderTextureVulkanImage* image,
                        const RenderTargetInfo& info, bool refresh) {
	UploadRenderTargetLayers(ctx, image, info, 0, info.layers, refresh);
}

RenderTextureVulkanImage* CreateRenderTarget(GraphicContext* ctx, const RenderTargetInfo& info,
                                             VulkanMemory* memory) {
	auto* image          = new RenderTextureVulkanImage;
	image->extent.width  = info.width;
	image->extent.height = info.height;
	image->format        = info.format;
	image->mip_levels    = info.levels;
	image->layers        = info.layers;
	image->layout        = VK_IMAGE_LAYOUT_UNDEFINED;
	UtilResetImageViews(image);
	VkImageCreateInfo create {};
	create.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	create.flags         = RenderTargetCreateFlags(info.format);
	create.imageType     = VK_IMAGE_TYPE_2D;
	create.extent        = {info.width, info.height, 1};
	create.mipLevels     = info.levels;
	create.arrayLayers   = info.layers;
	create.format        = info.format;
	create.tiling        = VK_IMAGE_TILING_OPTIMAL;
	create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	create.usage         = RenderTargetUsage(ctx, info.format, create.flags);
	create.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	create.samples       = VK_SAMPLE_COUNT_1_BIT;
	memory->property     = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	if (!VulkanCreateImage(ctx, &create, image, memory)) {
		EXIT("TextureCache: failed to create render target, addr=0x%016" PRIx64
		     " extent=%ux%u format=%d\n",
		     info.address, info.width, info.height, static_cast<int>(info.format));
	}
	image->memory = *memory;
	ImageViewOps::CreateRenderTargetViews(ctx, image);
	return image;
}

DepthStencilVulkanImage* CreateDepthTarget(GraphicContext* ctx, const DepthTargetInfo& info,
                                           VulkanMemory* memory) {
	VkImageCreateInfo create {};
	create.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	create.imageType     = VK_IMAGE_TYPE_2D;
	create.extent        = {info.width, info.height, 1};
	create.mipLevels     = 1;
	create.arrayLayers   = info.layers;
	create.format        = info.format;
	create.tiling        = VK_IMAGE_TILING_OPTIMAL;
	create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	create.usage         = DepthTargetImageUsage();
	create.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	create.samples       = VK_SAMPLE_COUNT_1_BIT;
	VkImageFormatProperties properties {};
	if (ctx->GetImageFormatProperties(info.format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
	                                  create.usage, 0, &properties) != VK_SUCCESS) {
		EXIT("TextureCache: depth format does not support required usage, format=%d usage=0x%x\n",
		     static_cast<int>(info.format), create.usage);
	}
	auto* image          = new DepthStencilVulkanImage;
	image->extent.width  = info.width;
	image->extent.height = info.height;
	image->guest_pitch   = info.pitch;
	image->layers        = info.layers;
	image->format        = info.format;
	image->layout        = VK_IMAGE_LAYOUT_UNDEFINED;
	image->compressed    = false;
	UtilResetImageViews(image);
	memory->property = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	if (!VulkanCreateImage(ctx, &create, image, memory)) {
		EXIT("TextureCache: failed to create depth target, addr=0x%016" PRIx64
		     " extent=%ux%u format=%d\n",
		     info.address, info.width, info.height, static_cast<int>(info.format));
	}
	image->memory = *memory;
	ImageViewOps::CreateDepthViews(ctx, image);
	return image;
}

void ValidateVideoOut(GraphicContext* ctx, const VideoOutInfo& info) {
	const auto compression =
	    ClassifyVideoOutCompression(info.compression != VideoOutCompression::Uncompressed,
	                                info.metadata_address, info.dcc_control, 0);
	const bool metadata_invalid = compression != VideoOutCompression::Uncompressed &&
	                              compression != VideoOutCompression::Unsupported &&
	                              (info.metadata_address >= TRACKER_ADDRESS_SIZE ||
	                               (info.metadata_address >= info.address &&
	                                info.metadata_address < info.address + info.size));
	if (ctx == nullptr || info.address == 0 || info.size == 0 ||
	    info.address >= TRACKER_ADDRESS_SIZE || info.size > TRACKER_ADDRESS_SIZE - info.address ||
	    (info.address & 0xffffu) != 0 || info.width == 0 || info.height == 0 ||
	    info.width > 16384 || info.height > 16384 || info.pitch < info.width ||
	    info.tile_mode != Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget) ||
	    compression == VideoOutCompression::Unsupported || compression != info.compression ||
	    metadata_invalid || !IsSupportedVideoOutFormat(info)) {
		EXIT("TextureCache: unsupported video-out surface, ctx=%p addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 " metadata=0x%016" PRIx64 " dcc=0x%08" PRIx32
		     " extent=%ux%u pitch=%u tile=%u guest_format=%u bpe=%u vk_format=%d\n",
		     static_cast<const void*>(ctx), info.address, info.size, info.metadata_address,
		     info.dcc_control, info.width, info.height, info.pitch, info.tile_mode,
		     info.guest_format, info.bytes_per_element, static_cast<int>(info.format));
	}
	TileSizeAlign exact {};
	TileGetTextureTotalSize(info.guest_format, info.width, info.height, 1, info.pitch, 1,
	                        info.tile_mode, false, &exact);
	if (exact.align != 65536 || exact.size != info.size ||
	    TileGetTexturePitch(info.guest_format, info.width, 1, info.tile_mode) != info.pitch) {
		EXIT("TextureCache: video-out tile layout mismatch, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 " expected_size=0x%016" PRIx64 " align=0x%016" PRIx64
		     " pitch=%u\n",
		     info.address, info.size, exact.size, exact.align, info.pitch);
	}
	(void)RenderTargetUsage(ctx, info.format, 0);
}

VideoOutVulkanImage* CreateVideoOut(GraphicContext* ctx, const VideoOutInfo& info,
                                    VulkanMemory* memory) {
	auto* image          = new VideoOutVulkanImage;
	image->extent.width  = info.width;
	image->extent.height = info.height;
	image->format        = info.format;
	image->layout        = VK_IMAGE_LAYOUT_UNDEFINED;
	UtilResetImageViews(image);
	VkImageCreateInfo create {};
	create.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	create.imageType     = VK_IMAGE_TYPE_2D;
	create.extent        = {info.width, info.height, 1};
	create.mipLevels     = 1;
	create.arrayLayers   = 1;
	create.format        = info.format;
	create.tiling        = VK_IMAGE_TILING_OPTIMAL;
	create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	create.flags         = RenderTargetCreateFlags(info.format);
	create.usage         = RenderTargetUsage(ctx, info.format, create.flags);
	create.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	create.samples       = VK_SAMPLE_COUNT_1_BIT;
	memory->property     = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	if (!VulkanCreateImage(ctx, &create, image, memory)) {
		EXIT("TextureCache: failed to create video-out image, addr=0x%016" PRIx64
		     " extent=%ux%u format=%d\n",
		     info.address, info.width, info.height, static_cast<int>(info.format));
	}
	image->memory = *memory;
	ImageViewOps::CreateVideoOutViews(ctx, image);
	return image;
}

void UploadVideoOut(GraphicContext* ctx, VideoOutVulkanImage* image, const VideoOutInfo& info,
                    bool refresh) {
	if (info.compression != VideoOutCompression::Uncompressed) {
		EXIT("TextureCache: compressed video-out guest upload is unsupported, "
		     "addr=0x%016" PRIx64 " metadata=0x%016" PRIx64 " dcc=0x%08" PRIx32 "\n",
		     info.address, info.metadata_address, info.dcc_control);
	}
	if (refresh) {
		VulkanDeviceWaitIdle(ctx);
	}
	image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
	UtilScratchBuffer scratch(info.size);
	TileConvertTiledToLinearRenderTarget(
	    scratch.Data(), reinterpret_cast<const void*>(info.address), info.width, info.height,
	    info.pitch, info.bytes_per_element, info.size);
	if (info.bgra16) {
		auto* pixels = static_cast<uint16_t*>(scratch.Data());
		for (uint64_t i = 0; i < info.size / sizeof(uint16_t); i += 4) {
			std::swap(pixels[i], pixels[i + 2]);
		}
	}
	UtilFillImage(ctx, image, scratch.Data(), info.size, info.pitch,
	              static_cast<uint64_t>(VK_IMAGE_LAYOUT_GENERAL));
}

GpuTextureVulkanImage* CreateDummyTexture(GraphicContext* ctx, bool uint_format, bool image_3d,
                                          bool storage, VulkanMemory* memory) {
	if (memory == nullptr || memory->allocation != nullptr) {
		EXIT("TextureCache: invalid dummy texture memory slot, slot=%p allocation=%p storage=%d\n",
		     static_cast<const void*>(memory),
		     memory == nullptr ? nullptr : static_cast<const void*>(memory->allocation), storage);
	}
	auto* image  = storage ? static_cast<GpuTextureVulkanImage*>(new StorageTextureVulkanImage)
	                       : new TextureVulkanImage;
	auto  usage  = storage ? TextureFormatUsage::Storage : TextureFormatUsage::Sampled;
	auto  layout = storage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	auto  owner  = storage ? "DummyStorageTexture" : "DummySampledTexture";

	auto params     = MakeDummyTextureParams(uint_format, image_3d, usage, owner);
	auto components = TextureCreateImage(ctx, image, memory, params);

	static constexpr uint32_t zero = 0;
	UtilFillImage(ctx, image, &zero, sizeof(zero), 1, static_cast<uint64_t>(layout));
	TextureCreateImageViews(ctx, image, components, params.type, 0, params.base_level,
	                        params.levels, params.depth, params.allow_cube_view, params.view_usage);
	return image;
}

void Destroy(GraphicContext* ctx, GpuTextureVulkanImage* image, VulkanMemory* memory) {
	KYTY_PROFILER_BLOCK("TextureCache::DeleteGpuTexture");

	EXIT_IF(ctx == nullptr);
	EXIT_IF(image == nullptr);

	ImageViewOps::DestroyViews(ctx, image);
	VulkanDeleteImage(ctx, image, memory);

	switch (image->type) {
		case VulkanImageType::Texture: delete static_cast<TextureVulkanImage*>(image); break;
		case VulkanImageType::StorageTexture:
			delete static_cast<StorageTextureVulkanImage*>(image);
			break;
		default: EXIT("unsupported gpu texture image type: %d\n", static_cast<int>(image->type));
	}
}

void Destroy(GraphicContext* ctx, RenderTextureVulkanImage* image, VulkanMemory* memory) {
	KYTY_PROFILER_BLOCK("TextureCache::DeleteRenderTexture");

	EXIT_IF(ctx == nullptr);
	EXIT_IF(image == nullptr);

	g_render_ctx->GetFramebufferCache()->FreeFramebufferByColor(image);
	ImageViewOps::DestroyViews(ctx, image);
	VulkanDeleteImage(ctx, image, memory);
	delete image;
}

void Destroy(GraphicContext* ctx, DepthStencilVulkanImage* image, VulkanMemory* memory) {
	KYTY_PROFILER_BLOCK("TextureCache::DeleteDepthStencil");

	EXIT_IF(ctx == nullptr);
	EXIT_IF(image == nullptr);

	g_render_ctx->GetFramebufferCache()->FreeFramebufferByDepth(image);
	ImageViewOps::DestroyViews(ctx, image);
	VulkanDeleteImage(ctx, image, memory);
	delete image;
}

void Destroy(GraphicContext* ctx, VideoOutVulkanImage* image, VulkanMemory* memory) {
	KYTY_PROFILER_BLOCK("TextureCache::DeleteVideoOut");

	EXIT_IF(ctx == nullptr);
	EXIT_IF(image == nullptr);

	g_render_ctx->GetFramebufferCache()->FreeFramebufferByColor(image);
	ImageViewOps::DestroyViews(ctx, image);
	VulkanDeleteImage(ctx, image, memory);
	delete image;
}

} // namespace ImageOps

} // namespace Libs::Graphics
