#ifndef KYTY_COMMON_ENVIRONMENT_H_
#define KYTY_COMMON_ENVIRONMENT_H_

#include <cstdlib>

namespace Common::Environment {

// Preserves presence-based switches: even an empty value counts as set.
[[nodiscard]] inline bool IsSet(const char* name) noexcept {
	return std::getenv(name) != nullptr;
}

} // namespace Common::Environment

#endif // KYTY_COMMON_ENVIRONMENT_H_
