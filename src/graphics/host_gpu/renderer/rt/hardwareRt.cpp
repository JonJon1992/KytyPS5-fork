#include "graphics/host_gpu/renderer/rt/hardwareRt.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "gpu_rt_shaders/gpu_hardware_rt_spv.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace Libs::Graphics::RT {

bool BackendRequested() {
	const char* value = std::getenv("KYTY_HW_RT_BACKEND");
	return value != nullptr && std::strcmp(value, "1") == 0;
}

void DeviceFeatures::Enable(vk::PhysicalDevice physical, std::vector<const char*>& extensions, bool requested) {
	enabled = false;
	if (!requested) return;
	const auto available = EnumerateVulkan<vk::ExtensionProperties>("RT device extensions",
	    [&](uint32_t* count, vk::ExtensionProperties* data) {
		    return physical.enumerateDeviceExtensionProperties(nullptr, count, data);
	    });
	constexpr const char* required[] {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
	                                 VK_KHR_RAY_QUERY_EXTENSION_NAME,
	                                 VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
	for (const char* name: required)
		if (std::none_of(available.begin(), available.end(), [&](const auto& ext) {
			    return std::strcmp(ext.extensionName.data(), name) == 0;
		    })) return;
	vk::PhysicalDeviceBufferDeviceAddressFeatures address {};
	vk::PhysicalDeviceFeatures2 features {};
	features.pNext = &acceleration;
	acceleration.pNext = &query;
	query.pNext = &address;
	physical.getFeatures2(&features);
	enabled = acceleration.accelerationStructure && query.rayQuery && address.bufferDeviceAddress;
	// Querying features must not implicitly enable optional AS or capture/replay features.
	acceleration = {};
	query = {};
	if (!enabled) return;
	acceleration.accelerationStructure = VK_TRUE;
	query.rayQuery = VK_TRUE;
	vk::PhysicalDeviceProperties2 props {};
	props.pNext = &properties;
	physical.getProperties2(&props);
	for (const char* name: required)
		if (std::none_of(extensions.begin(), extensions.end(), [&](const char* ext) {
			    return std::strcmp(ext, name) == 0;
		    })) extensions.push_back(name);
}

const void* DeviceFeatures::Chain(const void* next) {
	if (!enabled) return next;
	acceleration.pNext = &query;
	query.pNext = const_cast<void*>(next);
	return &acceleration;
}

namespace {
using BufferPtr = std::unique_ptr<Buffer>;
constexpr auto Address = vk::BufferUsageFlagBits::eShaderDeviceAddress;
constexpr auto BuildInput = vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR;
constexpr auto AsStage = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR;
constexpr auto AsWrite = vk::AccessFlagBits2::eAccelerationStructureWriteKHR;
constexpr auto AsRead = vk::AccessFlagBits2::eAccelerationStructureReadKHR;

BufferPtr Allocate(RenderContext& context, size_t bytes, vk::BufferUsageFlags flags, MemoryUsage usage) {
	return std::make_unique<Buffer>(context.GetGraphics(), context.GetCommandScheduler(), usage,
	                               0, flags | Address, bytes);
}

struct Acceleration {
	vk::Device device;
	vk::AccelerationStructureKHR handle = nullptr;
	BufferPtr backing, scratch;
	vk::DeviceAddress scratch_address = 0;
	~Acceleration() { if (handle) device.destroyAccelerationStructureKHR(handle); }
	bool Create(RenderContext& context, vk::AccelerationStructureTypeKHR type,
	            const vk::AccelerationStructureGeometryKHR& geometry, uint32_t count) {
		device = context.GetGraphics().device;
		vk::AccelerationStructureBuildGeometryInfoKHR build {};
		build.type = type;
		build.flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
		build.geometryCount = 1;
		build.pGeometries = &geometry;
		vk::AccelerationStructureBuildSizesInfoKHR sizes {};
		device.getAccelerationStructureBuildSizesKHR(vk::AccelerationStructureBuildTypeKHR::eDevice,
		                                            &build, &count, &sizes);
		const uint64_t alignment = context.GetGraphics().hardware_rt_properties.minAccelerationStructureScratchOffsetAlignment;
		// Bound each experimental allocation before asking VMA for device memory.
		if (alignment == 0 || sizes.accelerationStructureSize == 0 || sizes.buildScratchSize == 0 ||
		    sizes.accelerationStructureSize > (128u << 20) || sizes.buildScratchSize > (128u << 20)) return false;
		backing = Allocate(context, sizes.accelerationStructureSize,
		                   vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR, MemoryUsage::DeviceLocal);
		scratch = Allocate(context, sizes.buildScratchSize + alignment - 1,
		                   vk::BufferUsageFlagBits::eStorageBuffer, MemoryUsage::DeviceLocal);
		scratch_address = (scratch->BufferDeviceAddress() + alignment - 1) & ~(alignment - 1);
		vk::AccelerationStructureCreateInfoKHR create {};
		create.buffer = backing->Handle();
		create.size = sizes.accelerationStructureSize;
		create.type = type;
		RequireVulkanSuccess(device.createAccelerationStructureKHR(&create, nullptr, &handle), "create hardware RT AS");
		return true;
	}
	void Build(vk::CommandBuffer command, vk::AccelerationStructureTypeKHR type,
	           const vk::AccelerationStructureGeometryKHR& geometry, uint32_t count) {
		vk::AccelerationStructureBuildGeometryInfoKHR build {};
		build.type = type;
		build.flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
		build.mode = vk::BuildAccelerationStructureModeKHR::eBuild;
		build.dstAccelerationStructure = handle;
		build.geometryCount = 1;
		build.pGeometries = &geometry;
		build.scratchData.deviceAddress = scratch_address;
		vk::AccelerationStructureBuildRangeInfoKHR range {};
		range.primitiveCount = count;
		const auto* ranges = &range;
		command.buildAccelerationStructuresKHR(1, &build, &ranges);
	}
};

struct SceneStorage {
	// AS handles are destroyed before their backing buffers, inputs outlive AS use.
	BufferPtr vertices, instances;
	Acceleration blas, tlas;
};
} // namespace

struct HardwareBackend::Scene {
	const void* owner = nullptr;
	std::shared_ptr<const Geometry> geometry;
	std::shared_ptr<SceneStorage> storage;
};
struct HardwareBackend::Query {
	const void* owner = nullptr;
	SceneLease scene;
	BufferPtr input, output;
	std::vector<Hit> hits;
	std::atomic<bool> complete {false};
};
struct HardwareBackend::Impl {
	RenderContext& context;
	PreparationCache preparation;
	SceneLease cached;
	Statistics stats;
	vk::DescriptorSetLayout descriptors = nullptr;
	vk::PipelineLayout layout = nullptr;
	vk::Pipeline pipeline = nullptr;
	explicit Impl(RenderContext& value): context(value) {}
	~Impl() {
		// RenderContext drains the scheduler before destroying its owned backend.
		auto device = context.GetGraphics().device;
		if (pipeline) device.destroyPipeline(pipeline);
		if (layout) device.destroyPipelineLayout(layout);
		if (descriptors) device.destroyDescriptorSetLayout(descriptors);
	}
	void Initialize() {
		if (pipeline) return;
		auto device = context.GetGraphics().device;
		const std::array<vk::DescriptorSetLayoutBinding, 3> bindings {{
		    {0, vk::DescriptorType::eAccelerationStructureKHR, 1, vk::ShaderStageFlagBits::eCompute},
		    {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
		    {2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute}}};
		vk::DescriptorSetLayoutCreateInfo info {};
		info.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
		info.bindingCount = uint32_t(bindings.size());
		info.pBindings = bindings.data();
		RequireVulkanSuccess(device.createDescriptorSetLayout(&info, nullptr, &descriptors), "hardware RT descriptors");
		const vk::PushConstantRange push {vk::ShaderStageFlagBits::eCompute, 0, sizeof(uint32_t)};
		vk::PipelineLayoutCreateInfo pipeline_layout {};
		pipeline_layout.setLayoutCount = 1;
		pipeline_layout.pSetLayouts = &descriptors;
		pipeline_layout.pushConstantRangeCount = 1;
		pipeline_layout.pPushConstantRanges = &push;
		RequireVulkanSuccess(device.createPipelineLayout(&pipeline_layout, nullptr, &layout), "hardware RT layout");
		auto module = CompileSPV(GPU_HARDWARE_RT_SPV, device);
		vk::ComputePipelineCreateInfo compute {};
		compute.stage.stage = vk::ShaderStageFlagBits::eCompute;
		compute.stage.module = module;
		compute.stage.pName = "main";
		compute.layout = layout;
		const auto result = device.createComputePipelines(nullptr, 1, &compute, nullptr, &pipeline);
		device.destroyShaderModule(module);
		RequireVulkanSuccess(result, "hardware RT pipeline");
	}
};

HardwareBackend::HardwareBackend(RenderContext& context): m_impl(std::make_unique<Impl>(context)) {}
HardwareBackend::~HardwareBackend() = default;
bool HardwareBackend::Enabled() const { return m_impl->context.GetGraphics().hardware_rt_enabled; }
void HardwareBackend::Clear() { m_impl->cached.reset(); m_impl->preparation.Clear(); }
HardwareBackend::Statistics HardwareBackend::Stats() const { return m_impl->stats; }

HardwareBackend::SceneLease HardwareBackend::Prepare(BvhSnapshot snapshot) {
	auto& self = *m_impl;
	const auto reject = [&]() -> SceneLease { Clear(); ++self.stats.rejected; return {}; };
	if (!Enabled() || !self.context.GetCommandScheduler().Active()) return reject();
	auto prepared = self.preparation.Prepare(std::move(snapshot));
	if (!prepared.geometry) return reject();
	if (prepared.change == Change::Reuse && self.cached) { ++self.stats.reuses; return self.cached; }
	auto scene = std::make_shared<Scene>();
	scene->owner = &self;
	scene->geometry = prepared.geometry;
	if (prepared.change == Change::Metadata && self.cached) {
		scene->storage = self.cached->storage;
		self.cached = scene;
		++self.stats.reuses;
		return scene;
	}
	const size_t count = prepared.geometry->primitives.size();
	auto& context = self.context;
	auto& graphics = context.GetGraphics();
	if (count == 0 || count > graphics.hardware_rt_properties.maxPrimitiveCount || count > (1u << 20)) return reject();
	auto storage = std::make_shared<SceneStorage>();
	const size_t bytes = prepared.geometry->vertices.size() * sizeof(Vertex);
	storage->vertices = Allocate(context, bytes, BuildInput, MemoryUsage::Upload);
	std::memcpy(storage->vertices->Mapped().data(), prepared.geometry->vertices.data(), bytes);
	storage->vertices->Flush(0, bytes);
	vk::AccelerationStructureGeometryKHR geometry {};
	geometry.geometryType = vk::GeometryTypeKHR::eTriangles;
	geometry.flags = vk::GeometryFlagBitsKHR::eOpaque;
	auto& triangle = geometry.geometry.triangles;
	triangle = {};
	triangle.vertexFormat = vk::Format::eR32G32B32Sfloat;
	triangle.vertexData.deviceAddress = storage->vertices->BufferDeviceAddress();
	triangle.vertexStride = sizeof(Vertex);
	triangle.maxVertex = uint32_t(prepared.geometry->vertices.size() - 1);
	triangle.indexType = vk::IndexType::eNoneKHR;
	if (!storage->blas.Create(context, vk::AccelerationStructureTypeKHR::eBottomLevel, geometry, uint32_t(count))) return reject();
	vk::AccelerationStructureDeviceAddressInfoKHR address {};
	address.accelerationStructure = storage->blas.handle;
	VkAccelerationStructureInstanceKHR instance {};
	instance.transform.matrix[0][0] = instance.transform.matrix[1][1] = instance.transform.matrix[2][2] = 1;
	instance.mask = 255;
	instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
	instance.accelerationStructureReference = graphics.device.getAccelerationStructureAddressKHR(&address);
	storage->instances = Allocate(context, sizeof(instance), BuildInput, MemoryUsage::Upload);
	std::memcpy(storage->instances->Mapped().data(), &instance, sizeof(instance));
	storage->instances->Flush(0, sizeof(instance));
	vk::AccelerationStructureGeometryKHR instances {};
	instances.geometryType = vk::GeometryTypeKHR::eInstances;
	instances.geometry.instances = {};
	instances.geometry.instances.data.deviceAddress = storage->instances->BufferDeviceAddress();
	if (!storage->tlas.Create(context, vk::AccelerationStructureTypeKHR::eTopLevel, instances, 1)) return reject();
	scene->storage = storage;
	auto& scheduler = context.GetCommandScheduler();
	auto& command = scheduler.Current();
	command.EndRendering();
	command.RequestMemoryBarrier(vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostWrite,
	                             AsStage, vk::AccessFlagBits2::eShaderRead, BarrierOrigin::ShaderAccess);
	storage->blas.Build(command.Handle(), vk::AccelerationStructureTypeKHR::eBottomLevel, geometry, uint32_t(count));
	command.RequestMemoryBarrier(AsStage, AsWrite, AsStage, AsRead, BarrierOrigin::ShaderAccess);
	storage->tlas.Build(command.Handle(), vk::AccelerationStructureTypeKHR::eTopLevel, instances, 1);
	command.RequestMemoryBarrier(AsStage, AsWrite, vk::PipelineStageFlagBits2::eComputeShader, AsRead, BarrierOrigin::ShaderAccess);
	scheduler.DeferOperation([scene] {});
	self.cached = scene;
	++self.stats.builds;
	return scene;
}

HardwareBackend::QueryLease HardwareBackend::Trace(const SceneLease& scene, std::span<const Ray> rays) {
	auto& self = *m_impl;
	auto& graphics = self.context.GetGraphics();
	if (!Enabled() || !self.context.GetCommandScheduler().Active() || !scene || scene->owner != &self || rays.empty() || rays.size() > (1u << 20) ||
	    (rays.size() + 63) / 64 > graphics.physical_device_properties.limits.maxComputeWorkGroupCount[0]) return {};
	for (const auto& ray: rays) {
		if (!std::isfinite(ray.min_t) || !std::isfinite(ray.max_t) || ray.min_t < 0 || ray.max_t < ray.min_t ||
		    (ray.direction[0] == 0 && ray.direction[1] == 0 && ray.direction[2] == 0)) return {};
		for (unsigned i = 0; i < 3; ++i)
			if (!std::isfinite(ray.origin[i]) || !std::isfinite(ray.direction[i])) return {};
	}
	self.Initialize();
	auto query = std::make_shared<Query>();
	query->owner = &self;
	query->scene = scene;
	query->hits.resize(rays.size());
	query->input = Allocate(self.context, rays.size_bytes(), vk::BufferUsageFlagBits::eStorageBuffer, MemoryUsage::Upload);
	query->output = Allocate(self.context, rays.size() * sizeof(Hit), vk::BufferUsageFlagBits::eStorageBuffer, MemoryUsage::Download);
	std::memcpy(query->input->Mapped().data(), rays.data(), rays.size_bytes());
	query->input->Flush(0, rays.size_bytes());
	auto& scheduler = self.context.GetCommandScheduler();
	auto& command = scheduler.Current();
	command.EndRendering();
	command.RequestMemoryBarrier(vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostWrite,
	                             vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageRead, BarrierOrigin::ShaderAccess);
	command.BindPipeline(vk::PipelineBindPoint::eCompute, self.pipeline);
	vk::WriteDescriptorSetAccelerationStructureKHR as {};
	as.accelerationStructureCount = 1;
	as.pAccelerationStructures = &scene->storage->tlas.handle;
	std::array<vk::DescriptorBufferInfo, 2> buffers {{
	    {query->input->Handle(), 0, query->input->Size()}, {query->output->Handle(), 0, query->output->Size()}}};
	std::array<vk::WriteDescriptorSet, 3> writes {};
	for (uint32_t i = 0; i < writes.size(); ++i) {
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = i ? vk::DescriptorType::eStorageBuffer : vk::DescriptorType::eAccelerationStructureKHR;
		if (i) writes[i].pBufferInfo = &buffers[i - 1]; else writes[i].pNext = &as;
	}
	// Handle() drains encoded commands, flushes pending barriers and invalidates push shadows.
	auto handle = command.Handle();
	// An AS write has pNext payload, so bypass the ordinary descriptor comparator
	// but invalidate its shadow before disturbing the guest's compute set.
	command.InvalidateDescriptors(vk::PipelineBindPoint::eCompute);
	handle.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, self.layout, 0, uint32_t(writes.size()), writes.data());
	const uint32_t count = uint32_t(rays.size());
	handle.pushConstants(self.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(count), &count);
	handle.dispatch((count + 63) / 64, 1, 1);
	command.RequestMemoryBarrier(vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
	                             vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead, BarrierOrigin::ShaderAccess);
	scheduler.DeferOperation([query] {
		query->output->Invalidate(0, query->output->Size());
		std::memcpy(query->hits.data(), query->output->Mapped().data(), query->output->Size());
		query->complete.store(true, std::memory_order_release);
	});
	++self.stats.queries;
	return query;
}

std::span<const Hit> HardwareBackend::Read(const QueryLease& query) {
	if (!query || query->owner != m_impl.get() || !query->complete.load(std::memory_order_acquire)) return {};
	return query->hits;
}
std::span<const Primitive> HardwareBackend::Primitives(const SceneLease& scene) const {
	if (!scene || scene->owner != m_impl.get()) return {};
	return scene->geometry->primitives;
}
} // namespace Libs::Graphics::RT
