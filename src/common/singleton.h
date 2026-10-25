#ifndef KYTY_COMMON_SINGLETON_H_
#define KYTY_COMMON_SINGLETON_H_

#include "common/common.h"

#include <cstddef>
#include <new>

namespace Common {

template <class T>
class Singleton {
public:
	static T* Instance() {
		alignas(T) static std::byte storage[sizeof(T)];
		static T* const instance = ::new (static_cast<void*>(storage)) T;
		return instance;
	}

	KYTY_CLASS_NO_COPY(Singleton);

protected:
	Singleton();
	~Singleton();
};

} // namespace Common

#endif /* KYTY_COMMON_SINGLETON_H_ */
