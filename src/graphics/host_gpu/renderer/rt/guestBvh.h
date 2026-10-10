#ifndef EMULATOR_GRAPHICS_GUEST_BVH_H_
#define EMULATOR_GRAPHICS_GUEST_BVH_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace Libs::Graphics::RT {

// An immutable, coherent copy made AFTER all CPU/GPU writers have completed.
// A raw guest pointer or a CPU copy of GPU-dirty backing is not a snapshot.
struct BvhSnapshot {
	std::array<uint32_t, 4> descriptor {};
	uint32_t root = 0;
	uint64_t address = 0;
	std::vector<std::byte> bytes;
	bool operator==(const BvhSnapshot&) const = default;
};
struct Vertex {
	float x, y, z;
	bool operator==(const Vertex&) const = default;
};
struct Primitive {
	uint32_t node; // Full BVH32 pointer, including triangle kind, for hit remapping.
	uint32_t flags;
	bool operator==(const Primitive&) const = default;
};
struct Geometry {
	std::vector<Vertex> vertices; // Three vertices per Vulkan primitive, no indices.
	std::vector<Primitive> primitives;
	// Node pointer and four children for each box, in traversal order.
	std::vector<std::array<uint32_t, 5>> topology;
};
enum class Reject {
	None, Descriptor, SnapshotRange, Budget, Unmapped, NodeBounds,
	UnsupportedNode, CycleOrSharedNode, NonFinite, Empty,
};
struct Conversion {
	Reject reject = Reject::None;
	Geometry geometry;
	uint32_t offending_node = 0;
	explicit operator bool() const { return reject == Reject::None; }
};
// Converts a BVH32 subtree's geometry, NOT its traversal semantics. Instances,
// procedural nodes and shared/cyclic graphs are rejected. Caller must still
// prove shader traversal/acceptance equivalence before replacing any guest code.
Conversion Convert(const BvhSnapshot& snapshot, size_t max_nodes = 1u << 20);

// These classify preparation differences only. Metadata can include changed box
// bounds: no classification authorizes replacement of guest traversal. Update
// requires a separately built AS with ALLOW_UPDATE and completed prior queries.
enum class Change { Reuse, Metadata, Update, Rebuild, Reject };
struct Prepared {
	Change change = Change::Reject;
	RT::Reject reject = RT::Reject::None;
	std::shared_ptr<const Geometry> geometry;
};
// Single-entry preparation cache, owned by one caller/thread. Exact snapshot
// comparison: no hash collision or stale epoch may authorize reuse. It retains
// at most one source snapshot (max_bytes bounds its size, not total allocations);
// external readers can retain immutable geometry.
// This does not own an AS or authorize in-place updates while queries are live.
class PreparationCache {
public:
	explicit PreparationCache(size_t max_bytes = 64u << 20): m_max_bytes(max_bytes) {}
	Prepared Prepare(BvhSnapshot snapshot);
	void Clear();
private:
	size_t m_max_bytes;
	BvhSnapshot m_snapshot;
	std::shared_ptr<const Geometry> m_geometry;
};

} // namespace Libs::Graphics::RT
#endif
