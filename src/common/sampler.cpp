#include "common/sampler.h"

#include "common/assert.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <timeapi.h>
#endif

namespace Common::Sampler {

#ifdef _WIN32

namespace {

const auto g_process_start = std::chrono::steady_clock::now();

struct Config {
	bool     enabled = false;
	uint64_t start   = 170;
	uint64_t seconds = 10;
};

const Config& GetConfig() {
	static const Config config = [] {
		Config c;
		const char* value = std::getenv("KYTY_SAMPLE");
		if (value == nullptr) {
			return c;
		}
		c.enabled = true;
		char* end = nullptr;
		c.start   = std::strtoull(value, &end, 10);
		if (end != nullptr && *end == ':') {
			c.seconds = std::strtoull(end + 1, nullptr, 10);
		}
		return c;
	}();
	return config;
}

struct Target {
	std::string name;
	HANDLE      handle     = nullptr;
	uintptr_t   stack_base = 0;
};

constexpr int MaxCallers = 4;

struct Sample {
	uint32_t target;
	DWORD64  rip;
	DWORD64  callers[MaxCallers]; // return addresses into the executable found on the stack
};

uintptr_t g_image_begin = 0, g_image_end = 0;

// A plausible return address: inside the executable, right after a call instruction.
bool IsReturnAddress(uintptr_t a) {
	if (a < g_image_begin + 16 || a >= g_image_end) return false;
	const auto* code = reinterpret_cast<const uint8_t*>(a);
	if (code[-5] == 0xE8) return true;                                // call rel32
	if (code[-6] == 0xFF && (code[-5] & 0x38) == 0x10) return true;   // call [rip+disp32] / [r+disp32]
	if (code[-2] == 0xFF && (code[-1] & 0x38) == 0x10) return true;   // call r / [r]
	if (code[-3] == 0xFF && (code[-2] & 0x38) == 0x10) return true;   // call [r+disp8]
	if (code[-7] == 0xFF && (code[-6] & 0x38) == 0x10) return true;   // call [r+disp32] with SIB
	return false;
}

// Scans the top of a suspended thread's stack (up to its base) for return addresses.
void ScanStack(DWORD64 rsp, uintptr_t stack_base, DWORD64* callers) {
	const auto* stack = reinterpret_cast<const uintptr_t*>(rsp);
	int         found = 0;
	for (int k = 0; k < 512 && found < MaxCallers; k++) {
		if (rsp + (k + 1) * sizeof(uintptr_t) > stack_base) break;
		if (IsReturnAddress(stack[k])) callers[found++] = stack[k];
	}
}

std::mutex          g_mutex;
std::vector<Target> g_targets;
bool                g_started = false;

std::string FunctionOf(const std::string& symbol) {
	// " name+0xoffset [file:line]" -> "name"
	auto s = symbol;
	if (!s.empty() && s[0] == ' ') s.erase(0, 1);
	if (const auto plus = s.find("+0x"); plus != std::string::npos) s.resize(plus);
	return s.empty() ? "?" : s;
}

std::string FileOf(const std::string& symbol) {
	const auto open  = symbol.find(" [");
	const auto colon = symbol.rfind(':');
	if (open == std::string::npos || colon == std::string::npos || colon < open) return "?";
	auto file = symbol.substr(open + 2, colon - open - 2);
	if (const auto slash = file.find_last_of("\\/"); slash != std::string::npos) file.erase(0, slash + 1);
	return file;
}

void Run() {
	const auto& config = GetConfig();
	std::this_thread::sleep_until(g_process_start + std::chrono::seconds(config.start));
	std::vector<Target> targets;
	{
		std::scoped_lock lock(g_mutex);
		targets = g_targets;
	}
	if (targets.empty()) {
		return;
	}
	{
		MODULEINFO info {};
		if (GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &info, sizeof(info)) != FALSE) {
			g_image_begin = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
			g_image_end   = g_image_begin + info.SizeOfImage;
		}
	}
	timeBeginPeriod(1);
	// No allocation while a target is suspended (it may hold the heap lock).
	std::vector<Sample> samples;
	samples.reserve(static_cast<size_t>(config.seconds) * 1200u * targets.size() + 16u);
	const auto stop = std::chrono::steady_clock::now() + std::chrono::seconds(config.seconds);
	while (std::chrono::steady_clock::now() < stop && samples.size() + targets.size() < samples.capacity()) {
		for (uint32_t i = 0; i < targets.size(); i++) {
			if (SuspendThread(targets[i].handle) == static_cast<DWORD>(-1)) continue;
			CONTEXT context {};
			context.ContextFlags = CONTEXT_CONTROL;
			const bool ok        = GetThreadContext(targets[i].handle, &context) != FALSE;
			Sample     sample {i, context.Rip, {}};
			if (ok) {
				ScanStack(context.Rsp, targets[i].stack_base, sample.callers);
			}
			ResumeThread(targets[i].handle);
			if (ok) samples.push_back(sample);
		}
		Sleep(1);
	}
	timeEndPeriod(1);
	// Aggregate by function.
	std::unordered_map<DWORD64, std::string> names;
	for (uint32_t t = 0; t < targets.size(); t++) {
		std::map<std::string, uint64_t> by_function, by_file;
		uint64_t                        total = 0;
		for (const auto& s: samples) {
			if (s.target != t) continue;
			auto it = names.find(s.rip);
			if (it == names.end()) it = names.emplace(s.rip, Common::HostAddressName(reinterpret_cast<const void*>(s.rip))).first;
			by_function[FunctionOf(it->second)]++;
			by_file[FileOf(it->second)]++;
			total++;
		}
		std::vector<std::pair<std::string, uint64_t>> f(by_function.begin(), by_function.end()), g(by_file.begin(), by_file.end());
		std::sort(f.begin(), f.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
		std::sort(g.begin(), g.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
		std::string out = "[sample] thread " + targets[t].name + ": " + std::to_string(total) + " samples\n";
		char        line[512];
		for (size_t i = 0; i < f.size() && i < 45; i++) {
			std::snprintf(line, sizeof(line), "[sample]   %5.1f%%  %s\n", 100.0 * static_cast<double>(f[i].second) / static_cast<double>(std::max<uint64_t>(total, 1)), f[i].first.c_str());
			out += line;
		}
		// Callers of the hottest leaf functions (heuristic stack scan: may include stale frames).
		for (size_t i = 0; i < f.size() && i < 12; i++) {
			std::map<std::string, uint64_t> chains;
			uint64_t                        n = 0;
			for (const auto& s: samples) {
				if (s.target != t || FunctionOf(names[s.rip]) != f[i].first) continue;
				std::string chain;
				for (const auto c: s.callers) {
					if (c == 0) break;
					auto it = names.find(c);
					if (it == names.end()) it = names.emplace(c, Common::HostAddressName(reinterpret_cast<const void*>(c))).first;
					if (!chain.empty()) chain += " < ";
					chain += FunctionOf(it->second);
				}
				chains[chain.empty() ? "?" : chain]++;
				n++;
			}
			std::vector<std::pair<std::string, uint64_t>> c(chains.begin(), chains.end());
			std::sort(c.begin(), c.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
			out += "[sample]  callers of " + f[i].first + ":\n";
			for (size_t j = 0; j < c.size() && j < 5; j++) {
				std::snprintf(line, sizeof(line), "[sample]      %5.1f%%  %s\n", 100.0 * static_cast<double>(c[j].second) / static_cast<double>(std::max<uint64_t>(n, 1)), c[j].first.substr(0, 400).c_str());
				out += line;
			}
		}
		out += "[sample]  by file:";
		for (size_t i = 0; i < g.size() && i < 20; i++) {
			std::snprintf(line, sizeof(line), " %s %.1f%%", g[i].first.c_str(), 100.0 * static_cast<double>(g[i].second) / static_cast<double>(std::max<uint64_t>(total, 1)));
			out += line;
		}
		out += "\n";
		std::fputs(out.c_str(), stderr);
	}
	std::fflush(stderr);
}

// --- Freeze recorder ---

std::atomic<int64_t> g_last_frame_ms {0};
std::atomic<bool>    g_freeze_watch_started {false};

int64_t NowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
	           std::chrono::steady_clock::now() - g_process_start)
	    .count();
}

uint64_t ThreadCpuMs(HANDLE handle) {
	FILETIME create {}, exit {}, kernel {}, user {};
	if (GetThreadTimes(handle, &create, &exit, &kernel, &user) == FALSE) return 0;
	const auto k = (static_cast<uint64_t>(kernel.dwHighDateTime) << 32u) | kernel.dwLowDateTime;
	const auto u = (static_cast<uint64_t>(user.dwHighDateTime) << 32u) | user.dwLowDateTime;
	return (k + u) / 10000u;
}

constexpr int FreezeCallers = 14;

struct FreezeSample {
	bool    ok = false;
	DWORD64 rip = 0;
	DWORD64 callers[FreezeCallers] {};
};

// Suspends the thread, reads its instruction pointer and the return addresses on its stack, and
// resumes it; nothing is allocated meanwhile (it may hold the heap lock).
FreezeSample SampleThread(HANDLE handle) {
	FreezeSample sample;
	if (SuspendThread(handle) == static_cast<DWORD>(-1)) return sample;
	CONTEXT context {};
	context.ContextFlags = CONTEXT_CONTROL;
	if (GetThreadContext(handle, &context) != FALSE) {
		sample.ok  = true;
		sample.rip = context.Rip;
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<const void*>(context.Rsp), &info, sizeof(info)) != 0) {
			const auto top   = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
			const auto* slot = reinterpret_cast<const uintptr_t*>(context.Rsp);
			int         found = 0;
			for (int k = 0; k < 2048 && found < FreezeCallers; k++) {
				if (context.Rsp + (k + 1) * sizeof(uintptr_t) > top) break;
				if (IsReturnAddress(slot[k])) sample.callers[found++] = slot[k];
			}
		}
	}
	ResumeThread(handle);
	return sample;
}

bool InImage(DWORD64 address) {
	return address >= g_image_begin && address < g_image_end;
}

std::string NameOf(DWORD64 address) {
	if (address == 0) return "?";
	auto name = FunctionOf(Common::HostAddressName(reinterpret_cast<const void*>(address)));
	if (name == "?" || name.empty()) {
		char text[48];
		if (InImage(address)) {
			std::snprintf(text, sizeof(text), "exe+0x%llx", static_cast<unsigned long long>(address - g_image_begin));
		} else {
			std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
		}
		name = text;
	}
	return name;
}

void FreezeReport(int report) {
	struct Thread {
		DWORD    id;
		HANDLE   handle;
		uint64_t cpu0;
	};
	std::vector<Thread> threads;
	const DWORD         pid      = GetCurrentProcessId();
	const DWORD         self     = GetCurrentThreadId();
	HANDLE              snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snapshot == INVALID_HANDLE_VALUE) return;
	THREADENTRY32 entry {};
	entry.dwSize = sizeof(entry);
	for (BOOL more = Thread32First(snapshot, &entry); more != FALSE; more = Thread32Next(snapshot, &entry)) {
		if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) continue;
		HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
		                           FALSE, entry.th32ThreadID);
		if (handle != nullptr) threads.push_back({entry.th32ThreadID, handle, ThreadCpuMs(handle)});
	}
	CloseHandle(snapshot);
	Sleep(1000);
	std::string out;
	char        line[256];
	std::snprintf(line, sizeof(line), "[freeze] report %d: no guest flip for %lld s, %zu threads\n", report,
	              static_cast<long long>((NowMs() - g_last_frame_ms.load()) / 1000), threads.size());
	out += line;
	int listed = 0;
	for (auto& thread: threads) {
		const auto cpu  = ThreadCpuMs(thread.handle) - thread.cpu0;
		const bool busy = cpu >= 100;
		FreezeSample samples[3];
		samples[0] = SampleThread(thread.handle);
		bool ours  = samples[0].ok && InImage(samples[0].rip);
		for (const auto c: samples[0].callers) ours = ours || (c != 0 && InImage(c));
		if (!busy && (!ours || listed >= 48)) continue;
		listed++;
		if (busy) {
			Sleep(100);
			samples[1] = SampleThread(thread.handle);
			Sleep(100);
			samples[2] = SampleThread(thread.handle);
		}
		std::snprintf(line, sizeof(line), "[freeze] thread %lu cpu %llu ms/s%s\n", static_cast<unsigned long>(thread.id),
		              static_cast<unsigned long long>(cpu), busy ? " BUSY" : "");
		out += line;
		for (const auto& sample: samples) {
			if (!sample.ok) continue;
			std::string chain = "[freeze]   at " + NameOf(sample.rip);
			for (const auto c: sample.callers) {
				if (c == 0) break;
				chain += " < " + NameOf(c);
			}
			out += chain.substr(0, 1500) + "\n";
		}
	}
	for (auto& thread: threads) CloseHandle(thread.handle);
	std::fputs(out.c_str(), stderr);
	std::fflush(stderr);
}

void FreezeWatch() {
	{
		MODULEINFO info {};
		if (GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &info, sizeof(info)) != FALSE) {
			g_image_begin = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
			g_image_end   = g_image_begin + info.SizeOfImage;
		}
	}
	int     reports     = 0;
	int64_t next_report = 0;
	for (;;) {
		Sleep(1000);
		const auto last = g_last_frame_ms.load(std::memory_order_relaxed);
		const auto now  = NowMs();
		if (now - last < 15000) {
			next_report = 0;
			continue;
		}
		if (reports >= 3 || now < next_report) continue;
		for (int i = 0; i < 3 && reports < 3; i++) {
			FreezeReport(++reports);
		}
		next_report = now + 60000;
	}
}

} // namespace

void NoteGuestFrame() {
	g_last_frame_ms.store(NowMs(), std::memory_order_relaxed);
	if (!g_freeze_watch_started.load(std::memory_order_relaxed) &&
	    !g_freeze_watch_started.exchange(true)) {
		const char* value = std::getenv("KYTY_FREEZE_DUMP");
		if (value == nullptr || value[0] != '0') {
			std::thread(FreezeWatch).detach();
		}
	}
}

void RegisterCurrentThread(const char* name) {
	if (!GetConfig().enabled) {
		return;
	}
	HANDLE handle = nullptr;
	if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
	                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
	                    0) == FALSE) {
		return;
	}
	std::scoped_lock lock(g_mutex);
	const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
	g_targets.push_back({name, handle, reinterpret_cast<uintptr_t>(tib->StackBase)});
	if (!g_started) {
		g_started = true;
		std::thread(Run).detach();
	}
}

#else

void RegisterCurrentThread(const char* /*name*/) {}
void NoteGuestFrame() {}

#endif

} // namespace Common::Sampler
