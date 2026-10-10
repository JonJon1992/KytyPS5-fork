#include "graphics/host_gpu/renderer/rt/guestBvh.h"

#include <bit>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace Libs::Graphics::RT {
namespace {
constexpr uint64_t Aperture = 1ull << 40;

uint32_t Word(const BvhSnapshot& snapshot, size_t offset) {
	// Guest memory is little endian, regardless of the reader's byte order.
	uint32_t word = 0;
	for (unsigned byte = 0; byte < 4; ++byte)
		word |= uint32_t(std::to_integer<uint8_t>(snapshot.bytes[offset + byte])) << (byte * 8);
	return word;
}

bool SamePositions(const Geometry& a, const Geometry& b) {
	if (a.vertices.size() != b.vertices.size()) return false;
	for (size_t i = 0; i < a.vertices.size(); ++i) {
		const auto& x = a.vertices[i];
		const auto& y = b.vertices[i];
		if (std::bit_cast<uint32_t>(x.x) != std::bit_cast<uint32_t>(y.x) ||
		    std::bit_cast<uint32_t>(x.y) != std::bit_cast<uint32_t>(y.y) ||
		    std::bit_cast<uint32_t>(x.z) != std::bit_cast<uint32_t>(y.z)) return false;
	}
	return true;
}
} // namespace

Conversion Convert(const BvhSnapshot& snapshot, size_t max_nodes) {
	Conversion result;
	const auto fail = [&](Reject reason, uint32_t node = 0) {
		// A rejected conversion must never expose partially converted geometry.
		return Conversion {reason, {}, node};
	};
	const auto& d = snapshot.descriptor;
	if (snapshot.format != BvhFormat::DirectNodes && snapshot.format != BvhFormat::AstroLeafLists)
		return fail(Reject::Descriptor);
	if ((d[3] >> 28) != 8 || (d[1] & 0x007fff00u) || (d[3] & 0x0efffc00u))
		return fail(Reject::Descriptor);
	if (snapshot.address >= Aperture || snapshot.bytes.size() > Aperture - snapshot.address)
		return fail(Reject::SnapshotRange);
	const uint64_t base = (uint64_t(d[0]) | (uint64_t(d[1] & 0xff) << 32)) << 8;
	const uint64_t last = uint64_t(d[2]) | (uint64_t(d[3] & 0x3ff) << 32);
	const bool lists = snapshot.format == BvhFormat::AstroLeafLists;
	uint64_t geometry_bytes = 0, total_bytes = 0;
	if (lists) {
		if (base != snapshot.address || snapshot.bytes.size() < 96) return fail(Reject::Unmapped);
		if (Word(snapshot, 0) != 0x5f525350 || Word(snapshot, 4) != 0x4c485642)
			return fail(Reject::Descriptor);
		const uint64_t blocks = Word(snapshot, 80) | (uint64_t(Word(snapshot, 84)) << 32);
		const uint64_t records = Word(snapshot, 88) | (uint64_t(Word(snapshot, 92)) << 32);
		geometry_bytes = Word(snapshot, 32) | (uint64_t(Word(snapshot, 36)) << 32);
		total_bytes = Word(snapshot, 16) | (uint64_t(Word(snapshot, 20)) << 32);
		if (!blocks || blocks != last + 1 || blocks > Aperture / 64 ||
		    geometry_bytes != blocks * 64 || records > Aperture / 8 ||
		    total_bytes != records * 8 || total_bytes < geometry_bytes)
			return fail(Reject::Descriptor);
		if (total_bytes > snapshot.bytes.size()) return fail(Reject::Unmapped);
	}
	struct Entry { uint32_t node; uint32_t leaf_id = ~0u; bool triangle = false; };
	std::vector<Entry> pending {{snapshot.root}};
	std::unordered_set<uint32_t> seen;
	size_t work = 0;
	while (!pending.empty()) {
		const auto entry = pending.back();
		const uint32_t node = entry.node;
		pending.pop_back();
		if (node == ~0u) continue;
		if (work++ >= max_nodes) return fail(Reject::Budget, node);
		// Astro may reference the same triangle with different list IDs. Boxes
		// and list heads still cannot be shared or cyclic in this supported subset.
		if (!entry.triangle && !seen.insert(node).second) return fail(Reject::CycleOrSharedNode, node);
		const uint32_t kind = node & 7;
		if (kind > 5) return fail(Reject::UnsupportedNode, node);
		if (lists && kind < 4 && !entry.triangle) {
			// The shader's first node read uses pointer & ~4, ID uses
			// (pointer & ~6)+4. Only aligned list pointers are proven here.
			if (kind != 0) return fail(Reject::UnsupportedNode, node);
			if (node < geometry_bytes || node >= total_bytes) return fail(Reject::NodeBounds, node);
			std::vector<Entry> leaves;
			for (uint64_t at = node;; at += 8) {
				if (at > total_bytes || 8 > total_bytes - at) return fail(Reject::NodeBounds, node);
				if (work++ >= max_nodes) return fail(Reject::Budget, node);
				const auto pointer = Word(snapshot, size_t(at));
				const auto id = Word(snapshot, size_t(at + 4));
				if (leaves.empty() && (pointer & 7) == 7) break;
				if ((pointer & 7) >= 4) return fail(Reject::UnsupportedNode, pointer);
				leaves.push_back({pointer, id, true});
				// The terminator itself is tested and its raw ID is a valid hit ID.
				if (id & 0x80000000u) break;
			}
			for (auto it = leaves.rbegin(); it != leaves.rend(); ++it) pending.push_back(*it);
			continue;
		}
		const uint64_t index = node >> 3;
		const size_t length = kind == 5 ? 128 : 64;
		if (index + (kind == 5 ? 1 : 0) > last) return fail(Reject::NodeBounds, node);
		const uint64_t address = base + index * 64;
		if (address >= Aperture || length > Aperture - address)
			return fail(Reject::SnapshotRange, node);
		if (address < snapshot.address || address - snapshot.address > snapshot.bytes.size() ||
		    length > snapshot.bytes.size() - (address - snapshot.address))
			return fail(Reject::Unmapped, node);
		const auto offset = size_t(address - snapshot.address);
		if (kind < 4) {
			constexpr unsigned vertices[4][3] {{0, 1, 2}, {1, 3, 2}, {2, 3, 4}, {2, 4, 0}};
			for (unsigned vertex: vertices[kind]) {
				const size_t first = offset + vertex * 12;
				const Vertex position {std::bit_cast<float>(Word(snapshot, first)),
				                       std::bit_cast<float>(Word(snapshot, first + 4)),
				                       std::bit_cast<float>(Word(snapshot, first + 8))};
				if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
					return fail(Reject::NonFinite, node);
				result.geometry.vertices.push_back(position);
			}
			result.geometry.primitives.push_back({node, Word(snapshot, offset + 60), entry.leaf_id});
		} else {
			std::array<uint32_t, 5> topology {node};
			for (unsigned child = 0; child < 4; ++child) {
				topology[child + 1] = Word(snapshot, offset + child * 4);
				if (topology[child + 1] == ~0u) continue;
				for (unsigned axis = 0; axis < 6; ++axis) {
					// Check all bounds for live children, including packed FP16 exponents.
					if (kind == 4) {
						const auto packed = Word(snapshot, offset + (4 + child * 3 + axis / 2) * 4);
						if (((packed >> ((axis % 2) * 16)) & 0x7c00) == 0x7c00)
							return fail(Reject::NonFinite, node);
					} else if (!std::isfinite(std::bit_cast<float>(
					               Word(snapshot, offset + (4 + child * 6 + axis) * 4)))) {
						return fail(Reject::NonFinite, node);
					}
				}
			}
			result.geometry.topology.push_back(topology);
			// Reverse insertion preserves the guest's child order in the flattened IDs.
			for (unsigned i = 4; i > 0; --i)
				if (topology[i] != ~0u) pending.push_back({topology[i]});
		}
	}
	if (result.geometry.primitives.empty()) return fail(Reject::Empty);
	return result;
}

Prepared PreparationCache::Prepare(BvhSnapshot snapshot) {
	if (snapshot.bytes.size() > m_max_bytes) {
		Clear();
		return {Change::Reject, Reject::Budget, {}};
	}
	if (m_geometry && snapshot == m_snapshot) return {Change::Reuse, Reject::None, m_geometry};
	auto converted = Convert(snapshot);
	if (!converted) {
		Clear();
		return {Change::Reject, converted.reject, {}};
	}
	auto change = Change::Rebuild;
	if (m_geometry && snapshot.descriptor == m_snapshot.descriptor && snapshot.root == m_snapshot.root &&
	    snapshot.address == m_snapshot.address && snapshot.format == m_snapshot.format &&
	    converted.geometry.topology == m_geometry->topology &&
	    converted.geometry.primitives.size() == m_geometry->primitives.size()) {
		bool same_ids = true;
		for (size_t i = 0; i < m_geometry->primitives.size(); ++i)
			same_ids &= converted.geometry.primitives[i].node == m_geometry->primitives[i].node;
		if (same_ids) change = SamePositions(converted.geometry, *m_geometry) ? Change::Metadata : Change::Update;
	}
	m_geometry = std::make_shared<const Geometry>(std::move(converted.geometry));
	m_snapshot = std::move(snapshot);
	return {change, Reject::None, m_geometry};
}

void PreparationCache::Clear() {
	m_snapshot = {};
	m_geometry.reset();
}
} // namespace Libs::Graphics::RT
