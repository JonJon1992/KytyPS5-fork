#ifndef EMULATOR_SRC_COMMON_SLOTVECTOR_H_
#define EMULATOR_SRC_COMMON_SLOTVECTOR_H_

#include "common/assert.h"
#include "common/abi.h"

#include <compare>
#include <concepts>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace Common {

struct SlotId {
	static constexpr uint32_t INVALID_INDEX = std::numeric_limits<uint32_t>::max();

	constexpr SlotId() noexcept = default;
	constexpr SlotId(uint32_t value) noexcept: index(value), generation(1) {}
	constexpr SlotId(uint32_t value, uint32_t slot_generation) noexcept
	    : index(value), generation(slot_generation) {}

	[[nodiscard]] constexpr explicit operator bool() const noexcept { return index != INVALID_INDEX; }
	constexpr auto operator<=>(const SlotId&) const noexcept = default;

	uint32_t index = INVALID_INDEX;
	uint32_t generation = 0;
};

// Stable-address slot storage for cache resources that are intentionally non-movable.
template <typename T>
class SlotVector {
public:
	SlotVector() = default;
	KYTY_CLASS_NO_COPY(SlotVector);

	[[nodiscard]] T& operator[](SlotId id) noexcept {
		auto* value = try_get(id);
		EXIT_IF(value == nullptr);
		return *value;
	}

	[[nodiscard]] const T& operator[](SlotId id) const noexcept {
		const auto* value = try_get(id);
		EXIT_IF(value == nullptr);
		return *value;
	}

	[[nodiscard]] T* try_get(SlotId id) noexcept {
		if (!id || id.index >= m_dense_records.size()) {
			return nullptr;
		}
		auto& record = m_dense_records[id.index];
		return IsLive(record.state) && record.state == LiveState(id.generation) ? record.value : nullptr;
	}

	[[nodiscard]] const T* try_get(SlotId id) const noexcept {
		if (!id || id.index >= m_dense_records.size()) {
			return nullptr;
		}
		const auto& record = m_dense_records[id.index];
		return IsLive(record.state) && record.state == LiveState(id.generation) ? record.value : nullptr;
	}

	[[nodiscard]] bool is_allocated(SlotId id) const noexcept {
		return try_get(id) != nullptr;
	}

	template <typename... Args>
	[[nodiscard]] SlotId insert(Args&&... args) {
		uint32_t index = 0;
		if (m_free_list.empty()) {
			index = static_cast<uint32_t>(m_values.size());
			m_dense_records.emplace_back();
			try {
				m_values.emplace_back();
				m_values.back().value.emplace(std::forward<Args>(args)...);
			} catch (...) {
				if (m_values.size() > index) {
					m_values.pop_back();
				}
				m_dense_records.pop_back();
				throw;
			}
			auto& slot = m_values.back();
			m_dense_records.back() = {LiveState(slot.generation), &*slot.value};
		} else {
			index = m_free_list.back();
			auto& slot = m_values[index];
			EXIT_IF(slot.value.has_value());
			slot.value.emplace(std::forward<Args>(args)...);
			m_dense_records[index] = {LiveState(slot.generation), &*slot.value};
			m_free_list.pop_back();
		}
		++m_size;
		return SlotId {index, m_values[index].generation};
	}

	void erase(SlotId id) noexcept {
		EXIT_IF(!is_allocated(id));
		auto& slot = m_values[id.index];
		auto& record = m_dense_records[id.index];
		record = {DeadState(slot.generation), nullptr};
		slot.value.reset();
		if (++slot.generation == 0) {
			slot.generation = 1;
		}
		record.state = DeadState(slot.generation);
		m_free_list.push_back(id.index);
		--m_size;
	}

	[[nodiscard]] size_t size() const noexcept { return m_size; }
	[[nodiscard]] size_t capacity() const noexcept { return m_values.size(); }

	template <typename F>
		requires std::invocable<F&, SlotId, T&>
	void ForEach(F&& fn) {
		for (uint32_t index = 0; index < m_dense_records.size(); ++index) {
			const auto& record = m_dense_records[index];
			if (IsLive(record.state)) {
				EXIT_IF(record.value == nullptr);
				fn(SlotId {index, static_cast<uint32_t>(record.state)}, *record.value);
			}
		}
	}

	template <typename F>
		requires std::invocable<F&, SlotId, const T&>
	void ForEach(F&& fn) const {
		for (uint32_t index = 0; index < m_dense_records.size(); ++index) {
			const auto& record = m_dense_records[index];
			if (IsLive(record.state)) {
				EXIT_IF(record.value == nullptr);
				fn(SlotId {index, static_cast<uint32_t>(record.state)}, *record.value);
			}
		}
	}

private:
	struct Slot {
		std::optional<T> value;
		uint32_t         generation = 1;
	};
	// Keeps hot handle lookups independent of the deque node containing the (often large) value.
	struct DenseRecord {
		uint64_t state = 1;
		T*       value = nullptr;
	};
	static constexpr uint64_t LiveBit = uint64_t {1} << 32u;
	[[nodiscard]] static constexpr uint64_t LiveState(uint32_t generation) noexcept {
		return LiveBit | generation;
	}
	[[nodiscard]] static constexpr uint64_t DeadState(uint32_t generation) noexcept {
		return generation;
	}
	[[nodiscard]] static constexpr bool IsLive(uint64_t state) noexcept {
		return (state & LiveBit) != 0;
	}

	std::deque<Slot>          m_values;
	std::vector<DenseRecord>  m_dense_records;
	std::vector<uint32_t>     m_free_list;
	size_t                    m_size = 0;
};

} // namespace Common

template <>
struct std::hash<Common::SlotId> {
	[[nodiscard]] size_t operator()(Common::SlotId id) const noexcept {
		return std::hash<uint64_t> {}((static_cast<uint64_t>(id.generation) << 32u) | id.index);
	}
};

#endif // EMULATOR_SRC_COMMON_SLOTVECTOR_H_
