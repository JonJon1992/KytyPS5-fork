#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVBUILDER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVBUILDER_H_

#include "common/common.h"

#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <map>
#include <set>
#include <span>
#include <unordered_map>
#include <spirv/unified1/spirv.hpp>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

struct TypeAnnotation {
	spv::Op               opcode = spv::OpNop;
	std::vector<uint32_t> operands; // Already encoded operand words.
};

struct DeferredPhi {
	size_t word_offset    = 0;
};

class Builder {
public:
	explicit Builder(uint32_t version = 0x00010300u);
	~Builder() = default;
	KYTY_CLASS_DEFAULT_COPY(Builder);

	uint32_t AllocateId();
	void     RequireVersion(uint32_t version);
	void     RequireCapability(spv::Capability capability);
	void     RequireExtension(const char* name);
	uint32_t Import(const char* name);
	template <typename... Args>
	uint32_t Type(spv::Op opcode, const Args&... operands) {
		BuildTypeKey(opcode, operands...);
		return DeclareType(opcode);
	}

	template <typename... Args>
	uint32_t DecoratedType(spv::Op opcode, std::initializer_list<TypeAnnotation> annotations,
	                       const Args&... operands) {
		BuildTypeKey(opcode, operands...);
		return DeclareDecoratedType(opcode, annotations);
	}

	template <typename... Args>
	uint32_t Constant(spv::Op opcode, uint32_t type, const Args&... operands) {
		m_key.clear();
		AppendOperands(m_key, opcode, type, operands...);
		return DeclareConstant(opcode);
	}

	uint32_t DefineGlobalVariable(uint32_t pointer_type, spv::StorageClass storage_class);
	void DefineGlobalVariable(uint32_t id, uint32_t pointer_type, spv::StorageClass storage_class);

	void AddMemoryModel(spv::AddressingModel addressing_model, spv::MemoryModel memory_model);
	void AddEntryPoint(spv::ExecutionModel execution_model, uint32_t entry_point, const char* name,
	                   const std::vector<uint32_t>& interfaces);
	template <typename... Args>
	void AddExecutionMode(uint32_t entry_point, spv::ExecutionMode mode, const Args&... operands) {
		AppendInstruction(m_execution_modes, spv::OpExecutionMode, entry_point, mode, operands...);
	}

	void AddName(uint32_t target, const char* name);
	template <typename... Args>
	void AddAnnotation(spv::Op opcode, const Args&... operands) {
		AppendInstruction(m_annotations, opcode, operands...);
	}

	template <typename... Args>
	void AddFunction(spv::Op opcode, const Args&... operands) {
		AppendInstruction(m_functions, opcode, operands...);
	}

	void        AddFunction(std::span<const uint32_t> words);
	DeferredPhi AddDeferredPhi(uint32_t type, uint32_t result, size_t incoming_count);
	void        PatchDeferredPhi(DeferredPhi phi, size_t incoming, uint32_t value, uint32_t parent);

	[[nodiscard]] std::vector<uint32_t> Build() const;

private:
	static void AppendOperand(std::vector<uint32_t>& words, uint32_t value) {
		words.push_back(value);
	}

	static void AppendOperand(std::vector<uint32_t>& words, int32_t value) {
		words.push_back(static_cast<uint32_t>(value));
	}

	template <typename T>
	requires std::is_enum_v<T>
	static void AppendOperand(std::vector<uint32_t>& words, T value) {
		static_assert(sizeof(T) == sizeof(uint32_t));
		words.push_back(static_cast<uint32_t>(value));
	}

	static void AppendOperand(std::vector<uint32_t>& words, std::span<const uint32_t> values) {
		words.insert(words.end(), values.begin(), values.end());
	}

	template <typename... Args>
	static void AppendOperands(std::vector<uint32_t>& words, const Args&... operands) {
		(AppendOperand(words, operands), ...);
	}

	template <typename T>
	static size_t OperandWordCount(const T& operand) {
		if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {
			return 1;
		} else {
			return std::size(operand);
		}
	}

	// Declaration keys are built in m_key, reused across calls: a lookup that finds the
	// declaration allocates nothing.
	template <typename... Args>
	void BuildTypeKey(spv::Op opcode, const Args&... operands) {
		const auto operand_count = static_cast<uint32_t>((0u + ... + OperandWordCount(operands)));
		m_key.clear();
		AppendOperands(m_key, opcode, operand_count, operands...);
	}

	struct KeyHash {
		using is_transparent = void;
		size_t operator()(std::span<const uint32_t> key) const noexcept {
			uint64_t hash = 0xcbf29ce484222325ull;
			for (const auto word: key) {
				hash = (hash ^ word) * 0x100000001b3ull;
			}
			return static_cast<size_t>(hash ^ (hash >> 32u));
		}
	};
	struct KeyEqual {
		using is_transparent = void;
		bool operator()(std::span<const uint32_t> a, std::span<const uint32_t> b) const noexcept {
			return std::ranges::equal(a, b);
		}
	};

	template <typename... Args>
	static void AppendInstruction(std::vector<uint32_t>& section, spv::Op opcode,
	                              const Args&... operands) {
		const auto offset = section.size();
		AppendOperands(section, opcode, operands...);
		const auto word_count = static_cast<uint32_t>(section.size() - offset);
		section[offset] |= word_count << spv::WordCountShift;
	}

	uint32_t    DeclareType(spv::Op opcode);
	uint32_t    DeclareDecoratedType(spv::Op opcode,
	                                 std::initializer_list<TypeAnnotation> annotations);
	uint32_t    DeclareConstant(spv::Op opcode);
	static void AppendString(std::vector<uint32_t>& words, const char* text);

	uint32_t                                  m_next_id = 1;
	uint32_t                                  m_version = 0;
	std::vector<uint32_t>                     m_capabilities;
	std::vector<uint32_t>                     m_extensions;
	std::vector<uint32_t>                     m_ext_inst_imports;
	std::vector<uint32_t>                     m_memory_model;
	std::vector<uint32_t>                     m_entry_points;
	std::vector<uint32_t>                     m_execution_modes;
	std::vector<uint32_t>                     m_debug;
	std::vector<uint32_t>                     m_annotations;
	std::vector<uint32_t>                     m_declarations;
	std::vector<uint32_t>                     m_functions;
	std::set<spv::Capability>                 m_required_capabilities;
	std::set<std::string>                     m_required_extensions;
	std::map<std::string, uint32_t>           m_import_ids;
	std::unordered_map<std::vector<uint32_t>, uint32_t, KeyHash, KeyEqual> m_declaration_ids;
	std::vector<uint32_t>                     m_key;
	size_t                                    m_unpatched_phi_incomings = 0;
};

} // namespace Libs::Graphics::ShaderRecompiler::Spirv

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVBUILDER_H_ */
