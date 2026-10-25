// B1: driver compile time of the pipelines a session waited for (wait_shaders.py output), built
// the way the emulator builds them (shaders.cpp: set 0 = push descriptors when they fit, set 1 =
// the bindless table, a 128-byte push range, the guest wave size, DISABLE_OPTIMIZATION for the
// unoptimized builds), without any pipeline cache.
//
// Usage: pipe_bench <dir> <repeats> <u|o|uo> <variant>[,<variant>...] [same] [first] [end]
//   variant "-" is the recorded SPIR-V (<n>.ps.spv); "O" reads <n>.ps.O.spv, and so on.
//   u = DISABLE_OPTIMIZATION (what the waits were), o = an optimized build.
//   Every build gets a unique unused constant (a nonce) so the driver's own shader cache misses;
//   "same" leaves it out, to check that the nonce is what makes repeats compile again.
// Prints CSV: n,kind,variant,mode,rep,module_ms,pipeline_ms,words
//
// Build (VS x64 developer prompt):
//   clang-cl /O2 /std:c++20 /EHsc /I 3rdparty/Vulkan-Headers/include docs/benchmarks/pipe_bench.cpp
#define VK_NO_PROTOTYPES
#define NOMINMAX
#include <vulkan/vulkan.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#define VK_FUNCTIONS(X)                                                                                \
	X(vkCreateInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties2)              \
	X(vkGetPhysicalDeviceFeatures2) X(vkCreateDevice) X(vkCreateDescriptorSetLayout)                  \
	X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout)              \
	X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateGraphicsPipelines)                     \
	X(vkCreateComputePipelines) X(vkDestroyPipeline)
#define DECLARE(name) static PFN_##name name;
VK_FUNCTIONS(DECLARE)

static void Check(VkResult result, const char* what) {
	if (result != VK_SUCCESS) {
		std::fprintf(stderr, "%s failed: %d\n", what, static_cast<int>(result));
		std::exit(1);
	}
}

struct Binding {
	uint32_t set, binding;
	VkDescriptorType type;
	uint32_t count; // 0: runtime array
};

struct Io {
	uint32_t location;
	VkFormat format;
};

// What the pipeline needs to know about one SPIR-V module.
struct Module {
	std::vector<uint32_t> words;
	uint32_t              model = ~0u;
	std::vector<Binding>  bindings;
	std::vector<Io>       inputs, outputs;
	bool                  frag_depth = false;
	uint32_t              uint_type  = 0; // OpTypeInt 32 0, for the nonce
	size_t                first_function = 0;
};

static VkFormat IoFormat(const std::map<uint32_t, std::vector<uint32_t>>& types, uint32_t type,
                         bool attachment) {
	uint32_t components = 1;
	auto     it         = types.find(type);
	if (it != types.end() && (it->second[0] & 0xffffu) == 23) { // OpTypeVector
		components = it->second[3];
		it         = types.find(it->second[2]);
	}
	if (attachment) {
		components = 4;
	}
	const uint32_t op    = it->second[0] & 0xffffu;
	const bool     half  = it->second[2] == 16;
	const bool     sint  = op == 21 && it->second[3] == 1;
	const bool     uint  = op == 21 && !sint;
	static const VkFormat f32[] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT,
	                               VK_FORMAT_R32G32B32A32_SFLOAT};
	static const VkFormat u32[] = {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT,
	                               VK_FORMAT_R32G32B32A32_UINT};
	static const VkFormat s32[] = {VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT,
	                               VK_FORMAT_R32G32B32A32_SINT};
	static const VkFormat f16[] = {VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT,
	                               VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT};
	return (uint ? u32 : sint ? s32 : half ? f16 : f32)[components - 1];
}

static Module Load(const std::string& path) {
	Module        m;
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		std::fprintf(stderr, "cannot read %s\n", path.c_str());
		std::exit(1);
	}
	std::string bytes((std::istreambuf_iterator<char>(file)), {});
	m.words.resize(bytes.size() / 4);
	std::memcpy(m.words.data(), bytes.data(), m.words.size() * 4);

	std::map<uint32_t, std::vector<uint32_t>> types;
	std::map<uint32_t, uint32_t>              constants, sets, binds, locations, builtins;
	std::map<uint32_t, bool>                  buffer_block;
	struct Var {
		uint32_t type, id, storage;
	};
	std::vector<Var> vars;
	for (size_t at = 5; at < m.words.size();) {
		const uint32_t* w = &m.words[at];
		const uint32_t len = w[0] >> 16u, op = w[0] & 0xffffu;
		if (len == 0) {
			break;
		}
		switch (op) {
			case 15: m.model = w[1]; break;                                       // OpEntryPoint
			case 71:                                                              // OpDecorate
				if (w[2] == 34) sets[w[1]] = w[3];
				if (w[2] == 33) binds[w[1]] = w[3];
				if (w[2] == 30) locations[w[1]] = w[3];
				if (w[2] == 11) builtins[w[1]] = w[3];
				if (w[2] == 3) buffer_block[w[1]] = true;
				break;
			case 43: constants[w[2]] = w[3]; break;                               // OpConstant
			case 59: vars.push_back({w[1], w[2], w[3]}); break;                   // OpVariable
			case 54: if (m.first_function == 0) m.first_function = at; break;    // OpFunction
			default:
				if (op >= 19 && op <= 39) { // OpType*: id is the first operand
					types[w[1]] = std::vector<uint32_t>(w, w + len);
					if (op == 21 && w[2] == 32 && w[3] == 0) m.uint_type = w[1];
				}
		}
		at += len;
	}
	for (const auto& var: vars) {
		uint32_t type = types[var.type][3]; // OpTypePointer pointee
		if (var.storage == 1 || var.storage == 3) { // Input, Output
			if (builtins.count(var.id)) {
				m.frag_depth |= var.storage == 3 && builtins[var.id] == 22;
				continue;
			}
			if (!locations.count(var.id)) {
				continue;
			}
			uint32_t slots = 1;
			if ((types[type][0] & 0xffffu) == 28) { // OpTypeArray: one location per element
				slots = constants[types[type][3]];
				type  = types[type][2];
			}
			for (uint32_t i = 0; i < slots; i++) {
				const bool attachment = var.storage == 3 && m.model == 4;
				(var.storage == 1 ? m.inputs : m.outputs)
				    .push_back({locations[var.id] + i, IoFormat(types, type, attachment)});
			}
			continue;
		}
		if (var.storage != 0 && var.storage != 2 && var.storage != 12) {
			continue;
		}
		uint32_t count = 1;
		while (true) {
			const uint32_t op = types[type][0] & 0xffffu;
			if (op == 28) {
				count *= constants[types[type][3]];
			} else if (op == 29) {
				count = 0;
			} else {
				break;
			}
			type = types[type][2];
		}
		const auto&      t = types[type];
		VkDescriptorType kind;
		switch (t[0] & 0xffffu) {
			case 25: // OpTypeImage: Dim at 3, Sampled at 7
				kind = t[3] == 5 ? (t[7] == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER
				                               : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER)
				                 : (t[7] == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
				break;
			case 26: kind = VK_DESCRIPTOR_TYPE_SAMPLER; break;
			case 27: kind = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; break;
			case 30:
				kind = var.storage == 12 || buffer_block.count(type) ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
				                                                     : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
				break;
			default: std::fprintf(stderr, "%s: unknown descriptor type op %u\n", path.c_str(), t[0] & 0xffffu); std::exit(1);
		}
		m.bindings.push_back({sets[var.id], binds[var.id], kind, count});
	}
	return m;
}

// The module with an unused `OpConstant %uint <nonce>` before the first function, so the driver's
// shader cache (keyed by the module's bytes) does not recognise it.
static std::vector<uint32_t> WithNonce(const Module& m, uint32_t nonce) {
	std::vector<uint32_t> words(m.words.begin(), m.words.begin() + static_cast<long>(m.first_function));
	uint32_t              type = m.uint_type;
	if (type == 0) {
		type = words[3]++;
		words.insert(words.end(), {(4u << 16u) | 21u, type, 32u, 0u});
	}
	words.insert(words.end(), {(4u << 16u) | 43u, type, words[3]++, nonce});
	words.insert(words.end(), m.words.begin() + static_cast<long>(m.first_function), m.words.end());
	return words;
}

struct Device {
	VkDevice                         device = nullptr;
	VkPhysicalDeviceSubgroupSizeControlProperties subgroup {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
	VkPhysicalDevicePushDescriptorPropertiesKHR   push {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
	VkPhysicalDeviceVulkan11Properties            props11 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
};

// Every feature the device has in the structs the emulator enables from (robustness included,
// since it changes the generated code).
static Device CreateDevice() {
	HMODULE lib = LoadLibraryA("vulkan-1.dll");
	auto    gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(lib, "vkGetInstanceProcAddr"));
	vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
	VkApplicationInfo    app {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_3;
	VkInstanceCreateInfo ici {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	VkInstance instance;
	Check(vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance");
#define LOAD(name) name = reinterpret_cast<PFN_##name>(gipa(instance, #name));
	VK_FUNCTIONS(LOAD)

	uint32_t count = 0;
	vkEnumeratePhysicalDevices(instance, &count, nullptr);
	std::vector<VkPhysicalDevice> gpus(count);
	vkEnumeratePhysicalDevices(instance, &count, gpus.data());
	Device           d;
	VkPhysicalDevice gpu = nullptr;
	for (auto candidate: gpus) {
		VkPhysicalDeviceProperties2 p {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
		d.push.pNext     = &d.subgroup;
		d.subgroup.pNext = &d.props11;
		p.pNext          = &d.push;
		vkGetPhysicalDeviceProperties2(candidate, &p);
		if (p.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
			gpu = candidate;
			std::fprintf(stderr, "device: %s, driver 0x%08x\n", p.properties.deviceName, p.properties.driverVersion);
			break;
		}
	}
	if (gpu == nullptr) {
		std::fprintf(stderr, "no discrete GPU\n");
		std::exit(1);
	}

	VkPhysicalDeviceMeshShaderFeaturesEXT                     mesh {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
	VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT         atomic64 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT, &mesh};
	VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR  workgroup {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR, &atomic64};
	VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR      barycentric {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR, &workgroup};
	VkPhysicalDeviceRobustness2FeaturesEXT                    robustness2 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, &barycentric};
	VkPhysicalDeviceVulkan13Features                          f13 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &robustness2};
	VkPhysicalDeviceVulkan12Features                          f12 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
	VkPhysicalDeviceVulkan11Features                          f11 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
	VkPhysicalDeviceFeatures2                                 features {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
	vkGetPhysicalDeviceFeatures2(gpu, &features);
	// The emulator turns on meshShader only; the others need further extensions.
	mesh.taskShader = mesh.multiviewMeshShader = mesh.primitiveFragmentShadingRateMeshShader =
	    mesh.meshShaderQueries = VK_FALSE;
	atomic64.sparseImageInt64Atomics = VK_FALSE;

	const char* extensions[] = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,
	                            VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME,
	                            VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME,
	                            VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME,
	                            VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME, VK_EXT_MESH_SHADER_EXTENSION_NAME};
	const float             priority = 1.0f;
	VkDeviceQueueCreateInfo queue {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	queue.queueCount       = 1;
	queue.pQueuePriorities = &priority;
	VkDeviceCreateInfo dci {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &features};
	dci.queueCreateInfoCount    = 1;
	dci.pQueueCreateInfos       = &queue;
	dci.enabledExtensionCount   = static_cast<uint32_t>(std::size(extensions));
	dci.ppEnabledExtensionNames = extensions;
	Check(vkCreateDevice(gpu, &dci, nullptr, &d.device), "vkCreateDevice");
	return d;
}

static VkShaderStageFlagBits Stage(uint32_t model) {
	switch (model) {
		case 0: return VK_SHADER_STAGE_VERTEX_BIT;
		case 4: return VK_SHADER_STAGE_FRAGMENT_BIT;
		case 5: return VK_SHADER_STAGE_COMPUTE_BIT;
		case 5365: return VK_SHADER_STAGE_MESH_BIT_EXT;
		default: std::fprintf(stderr, "unsupported execution model %u\n", model); std::exit(1);
	}
}

// Set 0 and, when a stage uses it, set 1 (the bindless table), as shaders.cpp and bindlessTable.cpp make them.
static VkPipelineLayout CreateLayout(const Device& d, const std::vector<const Module*>& modules,
                                     std::vector<VkDescriptorSetLayout>& sets) {
	std::map<uint32_t, VkDescriptorSetLayoutBinding> set0;
	bool                                             bindless = false;
	VkShaderStageFlags                               stages   = 0;
	for (const Module* m: modules) {
		stages |= Stage(m->model);
		for (const auto& b: m->bindings) {
			if (b.set == 1) {
				bindless = true;
				continue;
			}
			auto& entry = set0[b.binding];
			entry       = {b.binding, b.type, b.count, entry.stageFlags | Stage(m->model), nullptr};
		}
	}
	std::vector<VkDescriptorSetLayoutBinding> bindings;
	uint32_t                                  total = 0;
	for (const auto& [_, b]: set0) {
		bindings.push_back(b);
		total += b.descriptorCount;
	}
	VkDescriptorSetLayoutCreateInfo info {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	info.flags        = total <= d.push.maxPushDescriptors ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0;
	info.bindingCount = static_cast<uint32_t>(bindings.size());
	info.pBindings    = bindings.data();
	sets.resize(1);
	Check(vkCreateDescriptorSetLayout(d.device, &info, nullptr, &sets[0]), "set 0 layout");
	if (bindless) {
		const uint32_t images = 65536, samplers = 4096; // BindlessTable::MaxImagesPerArray, MaxSamplers
		const VkShaderStageFlags all = VK_SHADER_STAGE_ALL;
		const VkDescriptorSetLayoutBinding table[] = {
		    {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, images, all, nullptr},
		    {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, images, all, nullptr},
		    {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, images, all, nullptr},
		    {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, images, all, nullptr},
		    {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, all, nullptr},
		    {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, all, nullptr},
		    {6, VK_DESCRIPTOR_TYPE_SAMPLER, samplers, all, nullptr}};
		const VkDescriptorBindingFlags image_flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
		                                             VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
		                                             VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
		const VkDescriptorBindingFlags flags[] = {image_flags, image_flags, image_flags, image_flags, 0, 0, image_flags};
		VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
		flags_info.bindingCount  = 7;
		flags_info.pBindingFlags = flags;
		VkDescriptorSetLayoutCreateInfo table_info {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &flags_info};
		table_info.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
		table_info.bindingCount = 7;
		table_info.pBindings    = table;
		sets.resize(2);
		Check(vkCreateDescriptorSetLayout(d.device, &table_info, nullptr, &sets[1]), "bindless layout");
	}
	const VkPushConstantRange  push {stages, 0, 128}; // sizeof(PushData)
	VkPipelineLayoutCreateInfo layout_info {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	layout_info.setLayoutCount         = static_cast<uint32_t>(sets.size());
	layout_info.pSetLayouts            = sets.data();
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges    = &push;
	VkPipelineLayout layout;
	Check(vkCreatePipelineLayout(d.device, &layout_info, nullptr, &layout), "pipeline layout");
	return layout;
}

using Clock = std::chrono::steady_clock;
static double Ms(Clock::time_point since) {
	return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
}

struct Timing {
	double module_ms = 0, pipeline_ms = 0;
};

// One build of the pipeline; `nonce` 0 keeps the modules as they are.
static Timing Build(const Device& d, const std::vector<const Module*>& modules, const std::vector<uint32_t>& waves,
                    bool unoptimized, uint32_t nonce) {
	Timing                             t;
	std::vector<VkDescriptorSetLayout> sets;
	VkPipelineLayout                   layout = CreateLayout(d, modules, sets);
	std::vector<VkShaderModule>        shader_modules;
	std::vector<VkPipelineShaderStageCreateInfo> stages;
	std::vector<VkPipelineShaderStageRequiredSubgroupSizeCreateInfo> sizes(modules.size());
	for (size_t i = 0; i < modules.size(); i++) {
		const auto words = nonce != 0 ? WithNonce(*modules[i], nonce + static_cast<uint32_t>(i)) : modules[i]->words;
		VkShaderModuleCreateInfo info {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		info.codeSize = words.size() * 4;
		info.pCode    = words.data();
		VkShaderModule module;
		const auto     start = Clock::now();
		Check(vkCreateShaderModule(d.device, &info, nullptr, &module), "vkCreateShaderModule");
		t.module_ms += Ms(start);
		shader_modules.push_back(module);
		VkPipelineShaderStageCreateInfo stage {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
		stage.stage  = Stage(modules[i]->model);
		stage.module = module;
		stage.pName  = "main";
		// shaders.cpp: compute always asks for the guest wave; fragment and mesh only when it is not
		// the default and the device lets the stage require it (GraphicsSubgroupSize).
		const bool compute = stage.stage == VK_SHADER_STAGE_COMPUTE_BIT;
		if ((compute || waves[i] != d.props11.subgroupSize) && (d.subgroup.requiredSubgroupSizeStages & stage.stage) &&
		    waves[i] >= d.subgroup.minSubgroupSize && waves[i] <= d.subgroup.maxSubgroupSize) {
			sizes[i]                      = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
			sizes[i].requiredSubgroupSize = waves[i];
			stage.pNext                   = &sizes[i];
		}
		stages.push_back(stage);
	}
	const VkPipelineCreateFlags flags = unoptimized ? VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT : 0;
	VkPipeline                  pipeline;
	if (modules[0]->model == 5) {
		VkComputePipelineCreateInfo info {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		info.flags  = flags;
		info.stage  = stages[0];
		info.layout = layout;
		const auto start = Clock::now();
		Check(vkCreateComputePipelines(d.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline), "compute pipeline");
		t.pipeline_ms = Ms(start);
	} else {
		const Module&                                  vs = *modules[0];
		const Module&                                  ps = *modules[1];
		std::vector<VkVertexInputBindingDescription>   vertex_bindings;
		std::vector<VkVertexInputAttributeDescription> attributes;
		if (vs.model == 0) {
			for (const auto& input: vs.inputs) {
				const auto binding = static_cast<uint32_t>(vertex_bindings.size());
				vertex_bindings.push_back({binding, 16, VK_VERTEX_INPUT_RATE_VERTEX});
				attributes.push_back({input.location, binding, input.format, 0});
			}
		}
		VkPipelineVertexInputStateCreateInfo vertex_input {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		vertex_input.vertexBindingDescriptionCount   = static_cast<uint32_t>(vertex_bindings.size());
		vertex_input.pVertexBindingDescriptions      = vertex_bindings.data();
		vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
		vertex_input.pVertexAttributeDescriptions    = attributes.data();
		VkPipelineInputAssemblyStateCreateInfo assembly {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		VkPipelineViewportStateCreateInfo viewport {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
		viewport.viewportCount = viewport.scissorCount = 1;
		VkPipelineRasterizationStateCreateInfo raster {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
		raster.lineWidth = 1.0f;
		VkPipelineMultisampleStateCreateInfo multisample {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
		multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineDepthStencilStateCreateInfo depth {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
		depth.depthTestEnable = depth.depthWriteEnable = ps.frag_depth ? VK_TRUE : VK_FALSE;
		depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
		uint32_t color_count = 0;
		for (const auto& output: ps.outputs) {
			color_count = std::max(color_count, output.location + 1);
		}
		std::vector<VkFormat> formats(color_count, VK_FORMAT_UNDEFINED);
		for (const auto& output: ps.outputs) {
			formats[output.location] = output.format;
		}
		std::vector<VkPipelineColorBlendAttachmentState> blend(color_count);
		for (auto& attachment: blend) {
			attachment.colorWriteMask = 0xf;
		}
		VkPipelineColorBlendStateCreateInfo color {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		color.attachmentCount = color_count;
		color.pAttachments    = blend.data();
		const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo dynamic {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
		dynamic.dynamicStateCount = 2;
		dynamic.pDynamicStates    = dynamic_states;
		VkPipelineRenderingCreateInfo rendering {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
		rendering.colorAttachmentCount    = color_count;
		rendering.pColorAttachmentFormats = formats.data();
		rendering.depthAttachmentFormat   = VK_FORMAT_D32_SFLOAT;
		VkGraphicsPipelineCreateInfo info {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
		info.flags               = flags;
		info.stageCount          = static_cast<uint32_t>(stages.size());
		info.pStages             = stages.data();
		info.pVertexInputState   = vs.model == 0 ? &vertex_input : nullptr;
		info.pInputAssemblyState = vs.model == 0 ? &assembly : nullptr;
		info.pViewportState      = &viewport;
		info.pRasterizationState = &raster;
		info.pMultisampleState   = &multisample;
		info.pDepthStencilState  = &depth;
		info.pColorBlendState    = &color;
		info.pDynamicState       = &dynamic;
		info.layout              = layout;
		const auto start = Clock::now();
		Check(vkCreateGraphicsPipelines(d.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline), "graphics pipeline");
		t.pipeline_ms = Ms(start);
	}
	vkDestroyPipeline(d.device, pipeline, nullptr);
	for (auto module: shader_modules) {
		vkDestroyShaderModule(d.device, module, nullptr);
	}
	vkDestroyPipelineLayout(d.device, layout, nullptr);
	for (auto set: sets) {
		vkDestroyDescriptorSetLayout(d.device, set, nullptr);
	}
	return t;
}

int main(int argc, char** argv) {
	if (argc < 5) {
		std::fprintf(stderr, "usage: pipe_bench <dir> <repeats> <u|o|uo> <variant>[,...] [same] [first] [end]\n");
		return 2;
	}
	const std::string dir     = argv[1];
	const int         repeats = std::atoi(argv[2]);
	const std::string modes   = argv[3];
	std::vector<std::string> variants;
	for (std::stringstream list(argv[4]); list.good();) {
		std::string v;
		std::getline(list, v, ',');
		variants.push_back(v);
	}
	int arg = 5;
	const bool same  = argc > arg && std::strcmp(argv[arg], "same") == 0 && ++arg;
	const int  first = argc > arg ? std::atoi(argv[arg]) : 0;
	const int  end   = argc > arg + 1 ? std::atoi(argv[arg + 1]) : 1 << 30;

	const Device d = CreateDevice();
	std::ifstream csv(dir + "/waits.csv");
	std::string   line;
	std::getline(csv, line); // n,kind,vs,ps,words,wave,ps_wave,...
	uint32_t nonce = 0xC0DE0000u + static_cast<uint32_t>(GetTickCount64() & 0xffffu) * 16u;
	std::printf("n,kind,variant,mode,rep,module_ms,pipeline_ms,words\n");
	while (std::getline(csv, line)) {
		std::vector<std::string> cols;
		for (std::stringstream row(line); row.good();) {
			cols.emplace_back();
			std::getline(row, cols.back(), ',');
		}
		const int n = std::atoi(cols[0].c_str());
		if (n < first || n >= end) {
			continue;
		}
		const bool                  compute = cols[1] == "CS";
		const std::vector<uint32_t> waves   = {static_cast<uint32_t>(std::atoi(cols[5].c_str())),
		                                       static_cast<uint32_t>(std::atoi(cols[6].c_str()))};
		for (const auto& variant: variants) {
			const std::string suffix = variant == "-" ? ".spv" : "." + variant + ".spv";
			std::vector<Module> owned;
			const auto          base = dir + "/" + std::to_string(n);
			if (compute) {
				owned.push_back(Load(base + ".cs" + suffix));
			} else {
				owned.push_back(Load(base + ".vs" + suffix));
				owned.push_back(Load(base + ".ps" + suffix));
			}
			std::vector<const Module*> modules;
			size_t                     words = 0;
			for (const auto& m: owned) {
				modules.push_back(&m);
				words += m.words.size();
			}
			for (char mode: modes) {
				for (int rep = 0; rep < repeats; rep++) {
					nonce += 4;
					const Timing t = Build(d, modules, waves, mode == 'u', same ? 0 : nonce);
					std::printf("%d,%s,%s,%c,%d,%.2f,%.2f,%zu\n", n, cols[1].c_str(), variant.c_str(), mode, rep,
					            t.module_ms, t.pipeline_ms, words);
					std::fflush(stdout);
				}
			}
		}
	}
	return 0;
}
