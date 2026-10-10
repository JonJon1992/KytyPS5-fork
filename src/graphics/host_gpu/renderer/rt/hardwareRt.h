#ifndef EMULATOR_GRAPHICS_HARDWARE_RT_H_
#define EMULATOR_GRAPHICS_HARDWARE_RT_H_

#include "graphics/host_gpu/renderer/rt/guestBvh.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include <span>

namespace Libs::Graphics {
class RenderContext;
namespace RT {

// Enables the native backend only; it does not authorize replacing guest code.
bool BackendRequested(); // KYTY_HW_RT_BACKEND=1, default off.
struct DeviceFeatures {
	bool enabled = false;
	vk::PhysicalDeviceAccelerationStructureFeaturesKHR acceleration {};
	vk::PhysicalDeviceRayQueryFeaturesKHR query {};
	vk::PhysicalDeviceAccelerationStructurePropertiesKHR properties {};
	void Enable(vk::PhysicalDevice physical, std::vector<const char*>& extensions, bool requested);
	const void* Chain(const void* next);
};

struct Ray {
	std::array<float, 3> origin;
	float min_t;
	std::array<float, 3> direction;
	float max_t;
};
struct Hit {
	float t, u, v;
	uint32_t primitive; // UINT32_MAX on miss. Native triangle coordinates, not guest numerators.
};
static_assert(sizeof(Ray) == 32 && sizeof(Hit) == 16);

// All calls and lease destruction belong to the renderer recording thread.
// Leases must be released before RenderContext destruction. The scheduler also
// retains every GPU-used resource until its submission has completed.
class HardwareBackend {
public:
	struct Scene;
	struct Query;
	using SceneLease = std::shared_ptr<const Scene>;
	using QueryLease = std::shared_ptr<Query>;
	~HardwareBackend();
	HardwareBackend(const HardwareBackend&) = delete;
	HardwareBackend& operator=(const HardwareBackend&) = delete;
	bool Enabled() const;
	// Requires coherent bytes as documented by BvhSnapshot. Never reads live guest backing.
	// Invalid input retires the cache; existing leases remain valid for older submissions.
	SceneLease Prepare(BvhSnapshot snapshot);
	QueryLease Trace(const SceneLease& scene, std::span<const Ray> rays);
	// Nonblocking. Empty until the scheduler has retired the query's completion callback.
	std::span<const Hit> Read(const QueryLease& query);
	std::span<const Primitive> Primitives(const SceneLease& scene) const;
	void Clear();
	struct Statistics { uint64_t builds = 0, reuses = 0, queries = 0, rejected = 0; };
	Statistics Stats() const;
private:
	// Only RenderContext may own the backend: it drains the scheduler before
	// destroying these pipelines. A standalone backend could die with live queries.
	friend class Libs::Graphics::RenderContext;
	explicit HardwareBackend(RenderContext& context);
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
} // namespace RT
} // namespace Libs::Graphics
#endif
