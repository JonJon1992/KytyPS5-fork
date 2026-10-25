#include "graphics/host_gpu/vulkanCommon.h"

#include "common/logging/log.h"
#include "graphics/host_gpu/aftermath.h"
#include "graphics/host_gpu/graphicContext.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <algorithm>
#include <mutex>
#include <vector>

namespace Libs::Graphics {

namespace {

constexpr size_t CHECKPOINT_RING_SIZE = 1u << 16u;

std::array<DiagnosticCheckpoint, CHECKPOINT_RING_SIZE> g_checkpoints {};
std::atomic<uint64_t>                                  g_checkpoint_sequence {0};

const char* OpName(uint32_t op) {
	switch (op) {
		case 0: return "DispatchDirect";
		case 1: return "DrawIndex";
		case 2: return "DrawIndexAuto";
		case 3: return "EopWrite";
		case 4: return "EopInterrupt";
		case 5: return "EopWriteBack";
		case 6: return "EopFlip";
		case 7: return "EopWriteBackFlip";
		case 8: return "EopOnlyFlip";
		case 9: return "DispatchIndirect";
		default: return "Unknown";
	}
}

void Print(const char* stage, const DiagnosticCheckpoint& checkpoint) {
	std::printf("  [%s] seq=%" PRIu64 " op=%s submit=%" PRIu64 " args=%u,%u,%u,%u,0x%016" PRIx64
	            "\n",
	            stage, checkpoint.sequence, OpName(checkpoint.op), checkpoint.submit_id,
	            checkpoint.arg0, checkpoint.arg1, checkpoint.arg2, checkpoint.arg3,
	            checkpoint.arg4);
	LOGF("  [%s] seq=%" PRIu64 " op=%s submit=%" PRIu64 " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	     stage, checkpoint.sequence, OpName(checkpoint.op), checkpoint.submit_id, checkpoint.arg0,
	     checkpoint.arg1, checkpoint.arg2, checkpoint.arg3, checkpoint.arg4);
}

} // namespace

const DiagnosticCheckpoint* RecordDiagnosticCheckpoint(const DiagnosticCheckpoint& checkpoint) {
	const auto sequence = g_checkpoint_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
	auto&      slot     = g_checkpoints[sequence % CHECKPOINT_RING_SIZE];
	slot                = checkpoint;
	slot.sequence       = sequence;
	return &slot;
}

namespace {

// KYTY_ADDRESS_BINDING_REPORT=1: the last device address bind/unbind events (VulkanAddressBinding
// Callback), newest last. A device fault reports a GPU virtual address; these say which object
// covered it and whether it had been freed, and how long before the fault.
struct AddressBindingEvent {
	uint64_t base        = 0;
	uint64_t size        = 0;
	uint64_t handle      = 0;
	uint64_t time_us     = 0;
	uint32_t object_type = 0;
	bool     bind        = false;
	bool     internal    = false;
};
constexpr size_t                                 ADDRESS_BINDING_RING_SIZE = 1u << 16u;
std::mutex                                       g_address_binding_mutex;
std::array<AddressBindingEvent, ADDRESS_BINDING_RING_SIZE> g_address_bindings {};
uint64_t                                         g_address_binding_count = 0;

uint64_t NowUs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

// The newest events whose range covers the address (within the fault's precision).
void PrintAddressBindingsAt(uint64_t address, uint64_t precision) {
	std::lock_guard lock(g_address_binding_mutex);
	if (g_address_binding_count == 0) {
		return;
	}
	const uint64_t now   = NowUs();
	const uint64_t slack = std::max<uint64_t>(precision, 1);
	const uint64_t kept  = std::min<uint64_t>(g_address_binding_count, ADDRESS_BINDING_RING_SIZE);
	uint32_t       shown = 0;
	for (uint64_t i = 0; i < kept && shown < 8; i++) {
		const auto& event = g_address_bindings[(g_address_binding_count - 1 - i) % ADDRESS_BINDING_RING_SIZE];
		if (address + slack <= event.base || address >= event.base + event.size + slack) {
			continue;
		}
		shown++;
		std::fprintf(stderr,
		             "[device-fault]   %s %s handle=0x%016" PRIx64 " 0x%016" PRIx64 "+0x%" PRIx64
		             "%s, %.1f ms before the report\n",
		             event.bind ? "bind" : "unbind",
		             vk::to_string(static_cast<vk::ObjectType>(event.object_type)).c_str(), event.handle,
		             event.base, event.size, event.internal ? " internal" : "",
		             static_cast<double>(now - event.time_us) / 1000.0);
	}
	if (shown == 0) {
		std::fprintf(stderr, "[device-fault]   no bind/unbind event of the last %" PRIu64
		                     " covers this address\n",
		             kept);
	}
}

} // namespace

void NoteAddressBinding(bool bind, uint64_t base, uint64_t size, uint32_t object_type,
                        uint64_t handle, bool internal) {
	std::lock_guard lock(g_address_binding_mutex);
	g_address_bindings[g_address_binding_count % ADDRESS_BINDING_RING_SIZE] = {
	    base, size, handle, NowUs(), object_type, bind, internal};
	g_address_binding_count++;
}

static void DumpDeviceFault(GraphicContext& graphics) {
	if (!graphics.device_fault_enabled) {
		return;
	}
	vk::DeviceFaultCountsEXT counts {};
	auto result = graphics.device.getFaultInfoEXT(&counts, nullptr);
	if (result != vk::Result::eSuccess && result != vk::Result::eIncomplete) {
		std::printf("--- Device fault: query failed: %s ---\n", vk::to_string(result).c_str());
		return;
	}
	std::vector<vk::DeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
	std::vector<vk::DeviceFaultVendorInfoEXT>  vendors(counts.vendorInfoCount);
	std::vector<uint8_t>                       binary(static_cast<size_t>(counts.vendorBinarySize));
	vk::DeviceFaultInfoEXT                     info {};
	info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
	info.pVendorInfos  = vendors.empty() ? nullptr : vendors.data();
	info.pVendorBinaryData = binary.empty() ? nullptr : binary.data();
	result = graphics.device.getFaultInfoEXT(&counts, &info);
	std::printf("--- Device fault (%s): \"%s\" addresses=%u vendor=%u binary=%" PRIu64 " ---\n",
	            vk::to_string(result).c_str(), info.description.data(), counts.addressInfoCount,
	            counts.vendorInfoCount, static_cast<uint64_t>(counts.vendorBinarySize));
	LOGF("--- Device fault (%s): \"%s\" addresses=%u vendor=%u binary=%" PRIu64 " ---\n",
	     vk::to_string(result).c_str(), info.description.data(), counts.addressInfoCount,
	     counts.vendorInfoCount, static_cast<uint64_t>(counts.vendorBinarySize));
	for (uint32_t i = 0; i < counts.addressInfoCount; i++) {
		const auto& address = addresses[i];
		std::printf("  address[%u]: %s at 0x%016" PRIx64 " (precision 0x%" PRIx64 ")\n", i,
		            vk::to_string(address.addressType).c_str(),
		            static_cast<uint64_t>(address.reportedAddress),
		            static_cast<uint64_t>(address.addressPrecision));
		std::fprintf(stderr, "[device-fault] address[%u]: %s at 0x%016" PRIx64 " (precision 0x%" PRIx64 ")\n", i,
		             vk::to_string(address.addressType).c_str(),
		             static_cast<uint64_t>(address.reportedAddress),
		             static_cast<uint64_t>(address.addressPrecision));
		PrintAddressBindingsAt(static_cast<uint64_t>(address.reportedAddress),
		                       static_cast<uint64_t>(address.addressPrecision));
		LOGF("  address[%u]: %s at 0x%016" PRIx64 " (precision 0x%" PRIx64 ")\n", i,
		     vk::to_string(address.addressType).c_str(),
		     static_cast<uint64_t>(address.reportedAddress),
		     static_cast<uint64_t>(address.addressPrecision));
	}
	for (uint32_t i = 0; i < counts.vendorInfoCount; i++) {
		const auto& vendor = vendors[i];
		std::printf("  vendor[%u]: \"%s\" code=0x%016" PRIx64 " data=0x%016" PRIx64 "\n", i,
		            vendor.description.data(), static_cast<uint64_t>(vendor.vendorFaultCode),
		            static_cast<uint64_t>(vendor.vendorFaultData));
		LOGF("  vendor[%u]: \"%s\" code=0x%016" PRIx64 " data=0x%016" PRIx64 "\n", i,
		     vendor.description.data(), static_cast<uint64_t>(vendor.vendorFaultCode),
		     static_cast<uint64_t>(vendor.vendorFaultData));
	}
	std::fflush(stdout);
	if (!binary.empty()) {
		if (auto* file = std::fopen("_device_fault.nv-gpudmp", "wb"); file != nullptr) {
			std::fwrite(binary.data(), 1, binary.size(), file);
			std::fclose(file);
			std::printf("  vendor binary written to _device_fault.nv-gpudmp\n");
		}
	}
	std::fflush(stdout);
}

// The draws and dispatches recorded per tick (CP side, always on): when a wait never finishes, the
// work of the first tick the GPU has not completed is named ([gpu-hang] work lines), without
// KYTY_NV_CHECKPOINTS (which serializes the GPU and hides the hang).
namespace {
struct TickWork {
	std::atomic<uint64_t> tick {0};
	uint64_t              a = 0, b = 0;
	char                  kind = 0;
};
constexpr size_t      TickWorkSize = 1u << 16u;
TickWork              g_tick_work[TickWorkSize];
std::atomic<uint64_t> g_tick_work_next {0};
} // namespace

void NoteTickWork(uint64_t tick, char kind, uint64_t a, uint64_t b) {
	auto& slot = g_tick_work[g_tick_work_next.fetch_add(1, std::memory_order_relaxed) % TickWorkSize];
	slot.tick.store(0, std::memory_order_relaxed);
	slot.a    = a;
	slot.b    = b;
	slot.kind = kind;
	slot.tick.store(tick, std::memory_order_release);
}

void DumpHangWork(uint64_t first_incomplete, uint64_t waited) {
	struct Entry {
		char     kind;
		uint64_t a, b;
		uint32_t count;
	};
	std::vector<Entry> entries;
	for (const auto& slot: g_tick_work) {
		const auto tick = slot.tick.load(std::memory_order_acquire);
		if (tick != first_incomplete) continue;
		auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) {
			return e.kind == slot.kind && e.a == slot.a && e.b == slot.b;
		});
		if (it == entries.end()) {
			entries.push_back({slot.kind, slot.a, slot.b, 1});
		} else {
			it->count++;
		}
	}
	std::fprintf(stderr, "[gpu-hang] first unfinished tick %" PRIu64 " (waited %" PRIu64 "): %zu distinct draws/dispatches\n",
	             first_incomplete, waited, entries.size());
	for (size_t i = 0; i < entries.size() && i < 60; i++) {
		std::fprintf(stderr, "[gpu-hang]   work %c %016" PRIx64 " %016" PRIx64 " x%u\n", entries[i].kind,
		             entries[i].a, entries[i].b, entries[i].count);
	}
	std::fflush(stderr);
}

// A wait that has not finished after seconds: the GPU's last checkpoints (the draw or dispatch it is
// stuck in), without losing the device. Requires KYTY_NV_CHECKPOINTS=1.
void DumpGpuHangCheckpoints(GraphicContext& graphics, uint64_t tick, double seconds) {
	if (!graphics.diagnostic_checkpoints_enabled || graphics.queue == nullptr) {
		std::fprintf(stderr, "[gpu-hang] tick %" PRIu64 " not done after %.0f s (KYTY_NV_CHECKPOINTS=1 names the work)\n",
		             tick, seconds);
		return;
	}
	std::vector<vk::CheckpointDataNV> data;
	{
		Common::LockGuard lock(graphics.queue_mutex);
		data = graphics.queue.getCheckpointDataNV();
	}
	std::fprintf(stderr, "[gpu-hang] tick %" PRIu64 " not done after %.0f s; checkpoints %zu, last recorded seq=%" PRIu64 "\n",
	             tick, seconds, data.size(), g_checkpoint_sequence.load(std::memory_order_relaxed));
	for (const auto& entry: data) {
		const auto* checkpoint = static_cast<const DiagnosticCheckpoint*>(entry.pCheckpointMarker);
		const bool  in_ring    = checkpoint >= g_checkpoints.data() &&
		                      checkpoint < g_checkpoints.data() + g_checkpoints.size();
		if (checkpoint == nullptr || !in_ring) {
			continue;
		}
		std::fprintf(stderr,
		             "[gpu-hang]   [%s] seq=%" PRIu64 " op=%s submit=%" PRIu64 " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
		             vk::to_string(entry.stage).c_str(), checkpoint->sequence, OpName(checkpoint->op),
		             checkpoint->submit_id, checkpoint->arg0, checkpoint->arg1, checkpoint->arg2,
		             checkpoint->arg3, checkpoint->arg4);
	}
	std::fflush(stderr);
}

void DumpDeviceLossDiagnostics(GraphicContext& graphics) {
	Aftermath::WaitForCrashDump();
	DumpDeviceFault(graphics);
	if (!graphics.diagnostic_checkpoints_enabled || graphics.queue == nullptr) {
		return;
	}
	std::vector<vk::CheckpointDataNV> data;
	{
		Common::LockGuard lock(graphics.queue_mutex);
		data = graphics.queue.getCheckpointDataNV();
	}
	std::printf("--- Diagnostic checkpoints (%zu, last recorded seq=%" PRIu64 ") ---\n",
	            data.size(), g_checkpoint_sequence.load(std::memory_order_relaxed));
	LOGF("--- Diagnostic checkpoints (%zu, last recorded seq=%" PRIu64 ") ---\n", data.size(),
	     g_checkpoint_sequence.load(std::memory_order_relaxed));
	for (const auto& entry: data) {
		const auto* checkpoint = static_cast<const DiagnosticCheckpoint*>(entry.pCheckpointMarker);
		const auto  stage      = vk::to_string(entry.stage);
		const bool  in_ring    = checkpoint >= g_checkpoints.data() &&
		                      checkpoint < g_checkpoints.data() + g_checkpoints.size();
		std::printf("  [%s] marker=%p ring=[%p, %p) %s\n", stage.c_str(),
		            static_cast<const void*>(checkpoint),
		            static_cast<const void*>(g_checkpoints.data()),
		            static_cast<const void*>(g_checkpoints.data() + g_checkpoints.size()),
		            in_ring ? "inside" : "OUTSIDE");
		if (checkpoint == nullptr || !in_ring) {
			continue;
		}
		Print(stage.c_str(), *checkpoint);
	}
	std::fflush(stdout);
}

} // namespace Libs::Graphics
