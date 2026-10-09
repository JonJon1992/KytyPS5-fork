#include "common/hangWatchdog.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

namespace {

constexpr size_t CHECKPOINT_RING_SIZE = 1u << 16u;

std::array<DiagnosticCheckpoint, CHECKPOINT_RING_SIZE> g_checkpoints {};
uint64_t g_checkpoint_sequence = 0;
std::mutex g_checkpoint_mutex;
std::atomic_flag g_dumping = ATOMIC_FLAG_INIT;

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

// NoteDiagnosticProgram: guest code address -> program hash and code size (the latest seen).
struct DiagnosticProgram {
	uint64_t hash = 0;
	uint64_t size = 0;
};
std::mutex                                      g_program_mutex;
std::unordered_map<uint64_t, DiagnosticProgram> g_programs;

DiagnosticProgram FindProgram(uint64_t address) {
	if (address == 0) {
		return {};
	}
	const std::lock_guard lock(g_program_mutex);
	const auto            found = g_programs.find(address);
	return found != g_programs.end() ? found->second : DiagnosticProgram {};
}

uint64_t ProgramHash(uint64_t address) {
	return FindProgram(address).hash;
}

// The guest code of the program at `address`, read from its backing (no fault), saved once per
// hash as device-loss-<hash>.bin for offline disassembly (shader_cfg_tests --structurize-file).
void SaveProgramCode(uint64_t address) {
	static std::vector<uint64_t> saved;
	const auto program = FindProgram(address);
	if (program.hash == 0 || program.size == 0 || program.size > 1024u * 1024u ||
	    std::ranges::find(saved, program.hash) != saved.end()) {
		return;
	}
	saved.push_back(program.hash);
	std::vector<uint8_t> code(static_cast<size_t>(program.size));
	if (!LibKernel::Memory::TryReadBacking(address, code.data(), code.size())) {
		std::printf("    program 0x%016" PRIx64 ": code not readable\n", program.hash);
		return;
	}
	char name[64];
	std::snprintf(name, sizeof(name), "device-loss-%016" PRIx64 ".bin", program.hash);
	if (auto* file = std::fopen(name, "wb"); file != nullptr) {
		std::fwrite(code.data(), 1, code.size(), file);
		std::fclose(file);
		std::printf("    saved %s (%" PRIu64 " bytes)\n", name, program.size);
	}
}

void Print(const char* stage, const DiagnosticCheckpoint& checkpoint) {
	std::printf("  [%s] seq=%" PRIu64 " op=%s submit=%" PRIu64 " args=%u,%u,%u,%u,0x%016" PRIx64
	            "\n",
	            stage, checkpoint.sequence, OpName(checkpoint.op), checkpoint.submit_id,
	            checkpoint.arg0, checkpoint.arg1, checkpoint.arg2, checkpoint.arg3,
	            checkpoint.arg4);
	std::printf("    tick=%" PRIu64 " shader VS=0x%016" PRIx64 " PS=0x%016" PRIx64 " CS=0x%016" PRIx64 "\n",
	            checkpoint.tick, checkpoint.vs, checkpoint.ps, checkpoint.cs);
	LOGF("    tick=%" PRIu64 " shader VS=0x%016" PRIx64 " PS=0x%016" PRIx64 " CS=0x%016" PRIx64 "\n",
	     checkpoint.tick, checkpoint.vs, checkpoint.ps, checkpoint.cs);
	std::printf("    program hash VS=0x%016" PRIx64 " PS=0x%016" PRIx64 " CS=0x%016" PRIx64 "\n",
	            ProgramHash(checkpoint.vs), ProgramHash(checkpoint.ps), ProgramHash(checkpoint.cs));
	for (const auto address: {checkpoint.vs, checkpoint.ps, checkpoint.cs}) {
		SaveProgramCode(address);
	}
	LOGF("  [%s] seq=%" PRIu64 " op=%s submit=%" PRIu64 " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	     stage, checkpoint.sequence, OpName(checkpoint.op), checkpoint.submit_id, checkpoint.arg0,
	     checkpoint.arg1, checkpoint.arg2, checkpoint.arg3, checkpoint.arg4);
}

// VK_AMD_buffer_marker: two host-coherent words, the sequence of the last checkpoint the GPU
// reached at the top of the pipe and of the last one whose earlier work completed (bottom). Created
// on the first marker (GPU thread), read after a loss; never freed (one per process).
struct MarkerWordsAMD {
	vk::Buffer                buffer;
	vk::DeviceMemory          memory;
	volatile const uint32_t*  words = nullptr;
	bool                      failed = false;
};
MarkerWordsAMD g_markers_amd;

bool CreateMarkersAMD(GraphicContext& graphics) {
	auto& markers = g_markers_amd;
	vk::BufferCreateInfo info {};
	info.size        = 2 * sizeof(uint32_t);
	info.usage       = vk::BufferUsageFlagBits::eTransferDst;
	info.sharingMode = vk::SharingMode::eExclusive;
	if (graphics.device.createBuffer(&info, nullptr, &markers.buffer) != vk::Result::eSuccess) {
		return false;
	}
	const auto requirements = graphics.device.getBufferMemoryRequirements(markers.buffer);
	const auto wanted = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
	const auto& types = graphics.physical_device_memory_properties;
	uint32_t type = UINT32_MAX;
	for (uint32_t index = 0; index < types.memoryTypeCount; ++index) {
		if ((requirements.memoryTypeBits & (1u << index)) != 0 &&
		    (types.memoryTypes[index].propertyFlags & wanted) == wanted) {
			type = index;
			break;
		}
	}
	vk::MemoryAllocateInfo allocate {};
	allocate.allocationSize  = requirements.size;
	allocate.memoryTypeIndex = type;
	void* mapped = nullptr;
	if (type == UINT32_MAX ||
	    graphics.device.allocateMemory(&allocate, nullptr, &markers.memory) != vk::Result::eSuccess) {
		return false;
	}
	if (graphics.device.bindBufferMemory(markers.buffer, markers.memory, 0) != vk::Result::eSuccess ||
	    graphics.device.mapMemory(markers.memory, 0, VK_WHOLE_SIZE, {}, &mapped) != vk::Result::eSuccess) {
		return false;
	}
	std::memset(mapped, 0, 2 * sizeof(uint32_t));
	markers.words = static_cast<volatile const uint32_t*>(mapped);
	return true;
}

void DumpMarkersAMD() {
	const auto& markers = g_markers_amd;
	if (markers.words == nullptr) {
		std::printf("  AMD buffer markers: none written\n");
		std::fflush(stdout);
		return;
	}
	const uint64_t started   = markers.words[0];
	const uint64_t completed = markers.words[1];
	std::printf("--- AMD buffer markers: last started seq=%" PRIu64
	            ", work before seq=%" PRIu64 " completed ---\n",
	            started, completed);
	LOGF("--- AMD buffer markers: last started seq=%" PRIu64 ", work before seq=%" PRIu64
	     " completed ---\n",
	     started, completed);
	// The hung work began at or after `completed` and at or before `started` (32-bit sequences).
	const uint64_t first = completed != 0 ? completed : started;
	const uint64_t last  = std::max(first, started);
	for (uint64_t sequence = first, shown = 0; sequence <= last && shown < 32; ++sequence, ++shown) {
		DiagnosticCheckpoint checkpoint;
		{
			const std::lock_guard lock(g_checkpoint_mutex);
			checkpoint = g_checkpoints[sequence % CHECKPOINT_RING_SIZE];
		}
		if (static_cast<uint32_t>(checkpoint.sequence) != static_cast<uint32_t>(sequence)) {
			std::printf("  seq=%" PRIu64 " retired from bounded history\n", sequence);
			continue;
		}
		Print(sequence == first ? "in flight (first)" : "in flight", checkpoint);
	}
	std::fflush(stdout);
}

} // namespace

void WriteDiagnosticMarkersAMD(GraphicContext& graphics, vk::CommandBuffer command, const void* marker) {
	auto& markers = g_markers_amd;
	if (markers.words == nullptr) {
		if (markers.failed) {
			return;
		}
		if (!CreateMarkersAMD(graphics)) {
			markers.failed = true;
			std::printf("AMD buffer markers: no host-coherent marker buffer; markers disabled\n");
			return;
		}
	}
	const auto sequence = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(marker));
	command.writeBufferMarkerAMD(vk::PipelineStageFlagBits::eTopOfPipe, markers.buffer, 0, sequence);
	command.writeBufferMarkerAMD(vk::PipelineStageFlagBits::eBottomOfPipe, markers.buffer,
	                             sizeof(uint32_t), sequence);
}

void NoteDiagnosticProgram(uint64_t address, uint64_t hash, uint64_t size_bytes) {
	const std::lock_guard lock(g_program_mutex);
	g_programs.insert_or_assign(address, DiagnosticProgram {hash, size_bytes});
}

bool DeviceFaultDiagnosticsEnabled() {
	static const bool enabled = [] {
		const auto* value = std::getenv("KYTY_DEVICE_FAULT_DIAGNOSTICS");
		return value != nullptr && std::strcmp(value, "0") != 0;
	}();
	return enabled;
}

const void* RecordDiagnosticCheckpoint(const DiagnosticCheckpoint& checkpoint) {
	const std::lock_guard lock(g_checkpoint_mutex);
	const auto sequence = ++g_checkpoint_sequence;
	auto& slot = g_checkpoints[sequence % CHECKPOINT_RING_SIZE];
	slot = checkpoint;
	slot.sequence = sequence;
	// Vulkan treats this as an opaque token. Never hand it a pointer to a reusable ring slot.
	return reinterpret_cast<const void*>(static_cast<uintptr_t>(sequence));
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
	if (counts.addressInfoCount > 4096 || counts.vendorInfoCount > 4096 || counts.vendorBinarySize > 64u * 1024u * 1024u) {
		std::printf("--- Device fault: driver returned oversized counts; skipping allocation ---\n");
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
	if (!binary.empty()) {
		if (auto* file = std::fopen("_device_fault.nv-gpudmp", "wb"); file != nullptr) {
			std::fwrite(binary.data(), 1, std::min<size_t>(binary.size(), counts.vendorBinarySize), file);
			std::fclose(file);
			std::printf("  vendor binary written to _device_fault.nv-gpudmp\n");
		}
	}
	for (uint32_t i = 0; i < std::min<size_t>(counts.addressInfoCount, addresses.size()); i++) {
		const auto& address = addresses[i];
		std::printf("  address[%u]: %s at 0x%016" PRIx64 " (precision 0x%" PRIx64 ")\n", i,
		            vk::to_string(address.addressType).c_str(),
		            static_cast<uint64_t>(address.reportedAddress),
		            static_cast<uint64_t>(address.addressPrecision));
		LOGF("  address[%u]: %s at 0x%016" PRIx64 " (precision 0x%" PRIx64 ")\n", i,
		     vk::to_string(address.addressType).c_str(),
		     static_cast<uint64_t>(address.reportedAddress),
		     static_cast<uint64_t>(address.addressPrecision));
	}
	for (uint32_t i = 0; i < std::min<size_t>(counts.vendorInfoCount, vendors.size()); i++) {
		const auto& vendor = vendors[i];
		std::printf("  vendor[%u]: \"%s\" code=0x%016" PRIx64 " data=0x%016" PRIx64 "\n", i,
		            vendor.description.data(), static_cast<uint64_t>(vendor.vendorFaultCode),
		            static_cast<uint64_t>(vendor.vendorFaultData));
		LOGF("  vendor[%u]: \"%s\" code=0x%016" PRIx64 " data=0x%016" PRIx64 "\n", i,
		     vendor.description.data(), static_cast<uint64_t>(vendor.vendorFaultCode),
		     static_cast<uint64_t>(vendor.vendorFaultData));
	}
	std::fflush(stdout);
}

void DumpDeviceLossDiagnostics(GraphicContext& graphics, uint64_t tick, bool queue_locked) {
	HangWatchdog::Scope diagnostics(
	    "device-loss-diagnostics",
	    reinterpret_cast<uint64_t>(static_cast<VkDevice>(graphics.device)), tick);
	if (g_dumping.test_and_set(std::memory_order_acquire)) return;
	std::printf("--- Device loss: submission/wait tick=%" PRIu64 ", KYTY_DEVICE_FAULT_DIAGNOSTICS=%d ---\n",
	            tick, DeviceFaultDiagnosticsEnabled());
	LOGF("--- Device loss: submission/wait tick=%" PRIu64 " ---\n", tick);
	DumpDeviceFault(graphics);
	if (graphics.amd_buffer_markers_enabled) {
		DumpMarkersAMD();
		return;
	}
	if (!graphics.diagnostic_checkpoints_enabled || graphics.queue == nullptr) {
		std::printf("  NV checkpoints unavailable; enable KYTY_DEVICE_FAULT_DIAGNOSTICS=1 before launch for supported driver diagnostics.\n");
		std::fflush(stdout);
		return;
	}
	std::vector<vk::CheckpointDataNV> data;
	if (queue_locked) {
		data = graphics.queue.getCheckpointDataNV();
	} else if (graphics.queue_mutex.TryLock()) {
		data = graphics.queue.getCheckpointDataNV();
		graphics.queue_mutex.Unlock();
	} else {
		// Fatal reporting must not wait behind another stalled driver submission.
		std::printf("  Queue busy: NV checkpoint query skipped; latest CPU breadcrumb follows (not GPU completion).\n");
		DiagnosticCheckpoint latest;
		{
			const std::lock_guard lock(g_checkpoint_mutex);
			latest = g_checkpoints[g_checkpoint_sequence % CHECKPOINT_RING_SIZE];
		}
		if (latest.sequence != 0) Print("latest CPU record", latest);
		std::fflush(stdout);
		return;
	}
	std::printf("--- Diagnostic checkpoints (%zu) ---\n", data.size());
	for (const auto& entry: data) {
		const auto sequence = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(entry.pCheckpointMarker));
		DiagnosticCheckpoint checkpoint;
		{
			const std::lock_guard lock(g_checkpoint_mutex);
			checkpoint = g_checkpoints[sequence % CHECKPOINT_RING_SIZE];
		}
		const auto stage = vk::to_string(entry.stage);
		if (sequence == 0 || checkpoint.sequence != sequence) {
			std::printf("  [%s] breadcrumb seq=%" PRIu64 " retired from bounded history\n", stage.c_str(), sequence);
			continue;
		}
		Print(stage.c_str(), checkpoint);
	}
	std::fflush(stdout);
}

} // namespace Libs::Graphics
