#include "graphics/host_gpu/renderer/sync.h"

#include "common/assert.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/objects/label.h"
#include "graphics/host_gpu/renderer/bufferCache.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/presentation/displayBuffer.h"
#include "kernel/eventQueue.h"
#include "kernel/pthread.h"
#include "libs/errno.h"

#include <limits>

namespace Libs::Graphics::Sync {

constexpr int      GRAPHICS_EVENT_QUEUED_GRAPHICS_INTERRUPT = 0x00;
constexpr int      GRAPHICS_EVENT_EOP                       = 0x40;
constexpr uint64_t GRAPHICS_REFERENCE_CLOCK_FREQUENCY       = 100000000;

bool ScaleReferenceClock(uint64_t host_ticks, uint64_t host_frequency, uint64_t* value) {
	if (host_frequency == 0 || value == nullptr) {
		return false;
	}

	const auto     whole_seconds = host_ticks / host_frequency;
	const auto     remainder     = host_ticks % host_frequency;
	constexpr auto MAX_VALUE     = std::numeric_limits<uint64_t>::max();
	if (whole_seconds > MAX_VALUE / GRAPHICS_REFERENCE_CLOCK_FREQUENCY ||
	    remainder > MAX_VALUE / GRAPHICS_REFERENCE_CLOCK_FREQUENCY) {
		return false;
	}

	const auto whole_value      = whole_seconds * GRAPHICS_REFERENCE_CLOCK_FREQUENCY;
	const auto fractional_value = (remainder * GRAPHICS_REFERENCE_CLOCK_FREQUENCY) / host_frequency;
	if (whole_value > MAX_VALUE - fractional_value) {
		return false;
	}
	*value = whole_value + fractional_value;
	return true;
}

uint64_t ReadReferenceClock() {
	const auto host_frequency = LibKernel::KernelGetTscFrequency();
	const auto host_ticks     = LibKernel::KernelReadTsc();
	uint64_t   value          = 0;
	if (!ScaleReferenceClock(host_ticks, host_frequency, &value)) {
		EXIT("cannot scale host clock, ticks=0x%016" PRIx64 " frequency=%" PRIu64 "\n", host_ticks,
		     host_frequency);
	}
	return value;
}

static void SubmitLabel(CommandBuffer* buffer, LabelCallback callback_1 = nullptr,
                        LabelCallback callback_2 = nullptr, const uint64_t* args = nullptr) {
	auto* label = LabelCreate(g_render_ctx->GetGraphicCtx(), callback_1, callback_2, args);
	LabelSet(buffer, label);
	LabelDelete(label);
}

static bool CompleteDisplayBufferFlip(const uint64_t* args) {
	if (g_render_ctx == nullptr || args == nullptr) {
		EXIT("GPU flip completion has invalid state, render_ctx=%p args=%p\n",
		     static_cast<const void*>(g_render_ctx), static_cast<const void*>(args));
	}
	Presentation::DisplayBufferCompleteFlipFromGpu(args[0]);
	return true;
}

void TriggerAgcUserInterrupt() {
	auto tsc    = LibKernel::KernelReadTsc();
	auto result = LibKernel::EventQueue::KernelTriggerUserEventForAll(AGC_USER_INTERRUPT_EVENT,
	                                                                  reinterpret_cast<void*>(tsc));
	EXIT_NOT_IMPLEMENTED(result != OK && result != LibKernel::KERNEL_ERROR_ENOENT);
}

void TriggerEopEvent(uint32_t context_id) {
	EXIT_IF(g_render_ctx == nullptr);
	g_render_ctx->TriggerEopEvent(context_id);
}

void WriteAtEndOfPipe32(uint64_t submit_id, CommandBuffer* buffer, uint32_t* dst_gpu_addr,
                        uint32_t value) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopWrite), submit_id, 4, value,
	                     0, 0, reinterpret_cast<uint64_t>(dst_gpu_addr));
}

void WriteAtEndOfPipeGds32(uint64_t submit_id, CommandBuffer* buffer, uint32_t* dst_gpu_addr,
                           uint32_t dw_offset, uint32_t dw_num) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopWrite), submit_id,
	                     dw_offset, dw_num, 0, 0, reinterpret_cast<uint64_t>(dst_gpu_addr));
}

void WriteAtEndOfPipe64(uint64_t submit_id, CommandBuffer* buffer, uint64_t* dst_gpu_addr,
                        uint64_t value) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopWrite), submit_id, 8,
	                     static_cast<uint32_t>(value), static_cast<uint32_t>(value >> 32u), 0,
	                     reinterpret_cast<uint64_t>(dst_gpu_addr));
}

void WriteAtEndOfPipeClockCounter(uint64_t submit_id, CommandBuffer* buffer, uint64_t* dst_gpu_addr,
                                  uint64_t value) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopWrite), submit_id, 8, 0, 0,
	                     0, reinterpret_cast<uint64_t>(dst_gpu_addr));

	LOGF_COLOR(Log::Color::BrightGreen,
	           "EndOfPipe Signal!!! [0x%016" PRIx64 "] <- Clock: 0x%016" PRIx64 "\n",
	           reinterpret_cast<uint64_t>(dst_gpu_addr), value);
}

void WriteAtEndOfPipeClockCounterWithWriteBack(uint64_t submit_id, CommandBuffer* buffer,
                                               uint64_t* dst_gpu_addr, uint64_t value) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopWriteBack), submit_id, 8, 0,
	                     0, 0, reinterpret_cast<uint64_t>(dst_gpu_addr));

	LOGF_COLOR(Log::Color::BrightGreen,
	           "EndOfPipe Signal!!! [0x%016" PRIx64 "] <- Clock: 0x%016" PRIx64 "\n",
	           reinterpret_cast<uint64_t>(dst_gpu_addr), value);
}

void WriteAtEndOfPipeWithWriteBack64(uint64_t submit_id, CommandBuffer* buffer,
                                     uint64_t* dst_gpu_addr, uint64_t value) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopWriteBack), submit_id, 8,
	                     static_cast<uint32_t>(value), static_cast<uint32_t>(value >> 32u), 0,
	                     reinterpret_cast<uint64_t>(dst_gpu_addr));
}

void WriteAtEndOfPipeWithWriteBack32(uint64_t submit_id, CommandBuffer* buffer,
                                     uint32_t* dst_gpu_addr, uint32_t value) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopWriteBack), submit_id, 4,
	                     value, 0, 0, reinterpret_cast<uint64_t>(dst_gpu_addr));
}

void WriteAtEndOfPipeWithInterruptWriteBack64(uint64_t submit_id, CommandBuffer* buffer,
                                              uint64_t* dst_gpu_addr, uint64_t value,
                                              uint32_t context_id) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopWriteBack), submit_id, 8,
	                     context_id, static_cast<uint32_t>(value),
	                     static_cast<uint32_t>(value >> 32u),
	                     reinterpret_cast<uint64_t>(dst_gpu_addr));
	uint64_t args[LABEL_ARGS_MAX] = {context_id != 0 ? context_id : value};

	SubmitLabel(
	    buffer, nullptr,
	    [](const uint64_t* args) {
		    EXIT_IF(g_render_ctx == nullptr);
		    g_render_ctx->TriggerEopEvent(static_cast<uint32_t>(args[0]));
		    return true;
	    },
	    args);
}

void WriteAtEndOfPipeWithInterruptWriteBack32(uint64_t submit_id, CommandBuffer* buffer,
                                              uint32_t* dst_gpu_addr, uint32_t value,
                                              uint32_t context_id) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopWriteBack), submit_id, 4,
	                     context_id, value, 0, reinterpret_cast<uint64_t>(dst_gpu_addr));
	uint64_t args[LABEL_ARGS_MAX] = {context_id != 0 ? context_id : value};

	SubmitLabel(
	    buffer, nullptr,
	    [](const uint64_t* args) {
		    EXIT_IF(g_render_ctx == nullptr);
		    g_render_ctx->TriggerEopEvent(static_cast<uint32_t>(args[0]));
		    return true;
	    },
	    args);
}

void WriteAtEndOfPipeWithInterrupt64(uint64_t submit_id, CommandBuffer* buffer,
                                     uint64_t* dst_gpu_addr, uint64_t value, uint32_t context_id) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopInterrupt), submit_id, 8,
	                     context_id, static_cast<uint32_t>(value),
	                     static_cast<uint32_t>(value >> 32u),
	                     reinterpret_cast<uint64_t>(dst_gpu_addr));
	uint64_t args[LABEL_ARGS_MAX] = {context_id != 0 ? context_id : value};

	SubmitLabel(
	    buffer, nullptr,
	    [](const uint64_t* args) {
		    EXIT_IF(g_render_ctx == nullptr);
		    g_render_ctx->TriggerEopEvent(static_cast<uint32_t>(args[0]));
		    return true;
	    },
	    args);
}

void WriteAtEndOfPipeWithInterrupt32(uint64_t submit_id, CommandBuffer* buffer,
                                     uint32_t* dst_gpu_addr, uint32_t value, uint32_t context_id) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopInterrupt), submit_id, 4,
	                     context_id, value, 0, reinterpret_cast<uint64_t>(dst_gpu_addr));
	uint64_t args[LABEL_ARGS_MAX] = {context_id != 0 ? context_id : value};

	SubmitLabel(
	    buffer, nullptr,
	    [](const uint64_t* args) {
		    EXIT_IF(g_render_ctx == nullptr);
		    g_render_ctx->TriggerEopEvent(static_cast<uint32_t>(args[0]));
		    return true;
	    },
	    args);
}

uint64_t PrepareDisplayBufferFlip(CommandBuffer* buffer, int handle, int index, int flip_mode,
                                  int64_t flip_arg) {
	for (;;) {
		uint64_t   request_id = 0;
		const auto result     = Presentation::DisplayBufferSubmitFlipFromGpu(
		    buffer, handle, index, flip_mode, flip_arg, &request_id);
		if (result == OK) {
			EXIT_IF(request_id == 0);
			return request_id;
		}
		if (result != VideoOut::VIDEO_OUT_ERROR_FLIP_QUEUE_FULL) {
			EXIT("GPU flip submission failed, result=%d handle=%d index=%d mode=%d arg=%" PRId64
			     "\n",
			     result, handle, index, flip_mode, flip_arg);
		}
		Presentation::DisplayBufferWaitForFlipQueueSlot();
	}
}

void WriteAtEndOfPipeWithInterruptWriteBackFlip32(uint64_t submit_id, CommandBuffer* buffer,
                                                  uint32_t* dst_gpu_addr, uint32_t value,
                                                  int handle, int index, int flip_mode,
                                                  int64_t flip_arg, uint64_t request_id) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopWriteBackFlip), submit_id,
	                     static_cast<uint32_t>(handle), static_cast<uint32_t>(index),
	                     static_cast<uint32_t>(flip_mode), value, static_cast<uint64_t>(flip_arg));
	const uint64_t args[LABEL_ARGS_MAX] = {request_id};
	SubmitLabel(
	    buffer, CompleteDisplayBufferFlip,
	    [](const uint64_t* /*args*/) {
		    EXIT_IF(g_render_ctx == nullptr);
		    g_render_ctx->TriggerEopEvent(0);
		    return true;
	    },
	    args);
}

void WriteAtEndOfPipeWithFlip32(uint64_t submit_id, CommandBuffer* buffer, uint32_t* dst_gpu_addr,
                                uint32_t value, int handle, int index, int flip_mode,
                                int64_t flip_arg, uint64_t request_id) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopFlip), submit_id,
	                     static_cast<uint32_t>(handle), static_cast<uint32_t>(index),
	                     static_cast<uint32_t>(flip_mode), value, static_cast<uint64_t>(flip_arg));
	const uint64_t args[LABEL_ARGS_MAX] = {request_id};
	SubmitLabel(buffer, CompleteDisplayBufferFlip, nullptr, args);
}

void WriteAtEndOfPipeOnlyFlip(uint64_t submit_id, CommandBuffer* buffer, int handle, int index,
                              int flip_mode, int64_t flip_arg, uint64_t request_id) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	buffer->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopOnlyFlip), submit_id,
	                     static_cast<uint32_t>(handle), static_cast<uint32_t>(index),
	                     static_cast<uint32_t>(flip_mode), 0, static_cast<uint64_t>(flip_arg));

	const uint64_t args[LABEL_ARGS_MAX] = {request_id};
	SubmitLabel(buffer, CompleteDisplayBufferFlip, nullptr, args);
}

void TriggerEopEventAtEndOfPipe(CommandBuffer* buffer, uint32_t context_id) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	uint64_t args[LABEL_ARGS_MAX] = {static_cast<uint64_t>(context_id)};

	SubmitLabel(
	    buffer, nullptr,
	    [](const uint64_t* args) {
		    EXIT_IF(g_render_ctx == nullptr);
		    g_render_ctx->TriggerEopEvent(static_cast<uint32_t>(args[0]));
		    return true;
	    },
	    args);
}

static void EopEventResetFunc(LibKernel::EventQueue::KernelEqueueEvent* event) {
	EXIT_IF(event == nullptr);
	event->triggered    = false;
	event->event.fflags = 0;
	event->event.data   = 0;
}

static void EopEventDeleteFunc(LibKernel::EventQueue::KernelEqueue       eq,
                               LibKernel::EventQueue::KernelEqueueEvent* event) {
	EXIT_IF(event == nullptr);
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_NOT_IMPLEMENTED(event->event.filter != LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS);
	if (event->event.ident == GRAPHICS_EVENT_QUEUED_GRAPHICS_INTERRUPT ||
	    event->event.ident == GRAPHICS_EVENT_EOP) {
		g_render_ctx->DeleteEopEq(eq, static_cast<int>(event->event.ident));
	}
}

static void EopEventTriggerFunc(LibKernel::EventQueue::KernelEqueueEvent* event,
                                void*                                     trigger_data) {
	EXIT_IF(event == nullptr);

	auto triggered_event = event->event;
	triggered_event.fflags++;
	triggered_event.data = reinterpret_cast<intptr_t>(trigger_data);
	if (event->triggered) {
		event->pending_events.push_back(triggered_event);
	} else {
		event->event     = triggered_event;
		event->triggered = true;
	}
}

int AddEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id, void* udata) {
	EXIT_IF(g_render_ctx == nullptr);

	LibKernel::EventQueue::KernelEqueueEvent event;
	event.triggered                = false;
	event.event.ident              = static_cast<uintptr_t>(id);
	event.event.filter             = LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS;
	event.event.udata              = udata;
	event.event.fflags             = 0;
	event.event.data               = id;
	event.filter.delete_event_func = EopEventDeleteFunc;
	event.filter.reset_func        = EopEventResetFunc;
	event.filter.trigger_func      = EopEventTriggerFunc;
	event.filter.data              = nullptr;

	int result = LibKernel::EventQueue::KernelAddEvent(eq, event);

	if (id == GRAPHICS_EVENT_QUEUED_GRAPHICS_INTERRUPT || id == GRAPHICS_EVENT_EOP) {
		g_render_ctx->AddEopEq(eq, id);
	}

	return result;
}

int DeleteEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id) {
	EXIT_IF(g_render_ctx == nullptr);

	int result = LibKernel::EventQueue::KernelDeleteEvent(
	    eq, static_cast<uintptr_t>(id), LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS);

	return result;
}

void ReadGds(uint32_t* dst, uint32_t dw_offset, uint32_t dw_size) {
	EXIT_IF(g_render_ctx == nullptr);

	g_render_ctx->GetGdsBuffer()->Read(g_render_ctx->GetGraphicCtx(), dst, dw_offset, dw_size);
}

void DeleteBuffers() {
	EXIT_IF(g_render_ctx == nullptr);
	g_render_ctx->GetBufferCache()->DeleteAll(g_render_ctx->GetGraphicCtx());
}

} // namespace Libs::Graphics::Sync
