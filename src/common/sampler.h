#ifndef KYTY_COMMON_SAMPLER_H_
#define KYTY_COMMON_SAMPLER_H_

#include <string>

namespace Common {
// " function+0xoffset [file:line]" of one host code address (empty without symbols); assert.cpp.
// Declared here rather than in assert.h: assert.h is part of the shader cache version.
std::string HostAddressName(const void* address);
} // namespace Common

namespace Common::Sampler {

// KYTY_SAMPLE=<start seconds>:<seconds> (Windows): a sampling profiler for the threads that
// register themselves. From <start> seconds after the emulator started, for <seconds>, every
// millisecond each registered thread is suspended, its instruction pointer read, and resumed;
// then the hottest functions of each thread are printed ([sample]). Without KYTY_SAMPLE,
// registering does nothing.
void RegisterCurrentThread(const char* name);

// Freeze recorder (Windows; KYTY_FREEZE_DUMP=0 turns it off): every guest flip calls
// NoteGuestFrame. When no flip came for 15 s after the game had been flipping, the threads of
// the process that are busy (and those stopped inside the emulator's own code) are suspended one
// at a time and their instruction pointer and return addresses are printed by name ([freeze]
// lines on stderr), three times a second apart; again after 60 s, at most three reports.
void NoteGuestFrame();

// Arms the freeze recorder at start-up, before the first flip, so a freeze while the game boots
// (no flip at all) is reported too, 15 s after this call.
void StartFreezeWatch();

} // namespace Common::Sampler

#endif /* KYTY_COMMON_SAMPLER_H_ */
