#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_SYSTEMCONTENT_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_SYSTEMCONTENT_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/stringUtils.h"

#include <cstdint>
#include <filesystem>

namespace Loader {

void     SystemContentLoadParamSfo(const std::filesystem::path& file_name);
// The host file the game executable (/app0/<elf>) is loaded from; known before /app0 is mounted.
void     SystemContentSetExecutablePath(const std::filesystem::path& host_path);
bool     SystemContentGetExecutablePath(std::filesystem::path* host_path);
bool     SystemContentParamSfoGetInt(const char* name, int32_t* value);
bool     SystemContentParamSfoGetString(const char* name, std::string* value);
bool     SystemContentParamSfoGetString(const char* name, char* value, size_t value_size);
uint64_t SystemContentGetFlexibleMemorySize();
bool     SystemContentGetIconPath(std::filesystem::path* path);
bool     SystemContentGetChunksNum(uint32_t* num);

} // namespace Loader

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_SYSTEMCONTENT_H_ */
