#ifndef KYTY_RENDERER_OCCLUSION_PAIRS_H_
#define KYTY_RENDERER_OCCLUSION_PAIRS_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Libs::Graphics {

// ZPASS dumps at A and A + 8 form an interleaved begin/end pair. A can be 8 mod 16,
// so the low address bits alone cannot distinguish the two dumps.
class OcclusionDumpPairs {
public:
	enum class Kind : uint8_t { Begin, RepeatedBegin, End };
	static constexpr size_t   MaxOpen     = 256;
	static constexpr uint64_t MaxAgeDumps = uint64_t {1} << 16u;

	[[nodiscard]] Kind Observe(uint64_t address) {
		const auto serial = m_serial++;
		ExpireBefore(serial);
		if (address >= 8u) {
			const auto end = Find(address - 8u);
			if (end != m_open.end()) {
				m_open.erase(end);
				return Kind::End;
			}
		}
		const auto repeated = Find(address);
		if (repeated != m_open.end()) {
			// A repeated begin replaces the earlier snapshot and refreshes its age.
			repeated->serial = serial;
			return Kind::RepeatedBegin;
		}
		if (m_open.size() == MaxOpen) {
			const auto oldest = std::min_element(m_open.begin(), m_open.end(),
			                                     [](const Open& a, const Open& b) { return a.serial < b.serial; });
			m_open.erase(oldest);
			++m_dropped;
		}
		m_open.push_back({address, serial});
		return Kind::Begin;
	}

	[[nodiscard]] bool     Empty() const noexcept { return m_open.empty(); }
	[[nodiscard]] size_t   OpenCount() const noexcept { return m_open.size(); }
	[[nodiscard]] uint64_t Dropped() const noexcept { return m_dropped; }

private:
	struct Open {
		uint64_t address;
		uint64_t serial;
	};
	[[nodiscard]] std::vector<Open>::iterator Find(uint64_t address) {
		return std::find_if(m_open.begin(), m_open.end(),
		                    [address](const Open& open) { return open.address == address; });
	}
	void ExpireBefore(uint64_t serial) {
		const auto old = std::remove_if(m_open.begin(), m_open.end(), [serial](const Open& open) {
			return serial - open.serial >= MaxAgeDumps;
		});
		m_dropped += static_cast<uint64_t>(m_open.end() - old);
		m_open.erase(old, m_open.end());
	}

	std::vector<Open> m_open;
	uint64_t m_serial  = 0;
	uint64_t m_dropped = 0;
};

} // namespace Libs::Graphics

#endif
