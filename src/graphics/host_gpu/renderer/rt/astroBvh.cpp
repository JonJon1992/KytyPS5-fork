#include "graphics/host_gpu/renderer/rt/astroBvh.h"
#include "graphics/shader/recompiler/BvhCapture.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <fstream>
#include "graphics/shader/recompiler/AstroNativeBinding.h"
namespace Libs::Graphics::RT {
std::optional<AstroInstance> DecodeAstroInstance(std::span<const std::byte> bytes,
                                                uint64_t address, bool embedded) {
	constexpr uint64_t aperture = 1ull << 40;
	if (bytes.size() != 160 || address >= aperture || 160 > aperture - address || (address & 3)) return {};
	const auto word = [&](size_t offset) {
		uint32_t value = 0;
		for (unsigned i = 0; i < 4; ++i)
			value |= uint32_t(std::to_integer<uint8_t>(bytes[offset + i])) << (i * 8);
		return value;
	};
	AstroInstance instance;
	instance.address = address;
	instance.flags = word(0);
	instance.blas_address = word(8) | (uint64_t(word(12)) << 32);
	instance.root = embedded ? 37u : word(152);
	if (instance.flags <= 255 || !instance.blas_address || instance.blas_address >= aperture ||
	    (instance.blas_address & 255) || instance.root == ~0u ||
	    ((instance.root & 7) != 0 && (instance.root & 7) != 4 && (instance.root & 7) != 5)) return {};
	for (unsigned row = 0; row < 3; ++row)
		for (unsigned column = 0; column < 4; ++column) {
			const size_t offset = column == 3 ? 64 + row * 4 : 16 + column * 16 + row * 4;
			const float value = std::bit_cast<float>(word(offset));
			if (!std::isfinite(value)) return {};
			instance.world_to_object[row * 4 + column] = value;
		}
	// Invert the affine world->object matrix in double precision. Vulkan stores
	// object->world; the shader's translation is already part of the inverse.
	const auto& m = instance.world_to_object;
	const double a=m[0], b=m[1], c=m[2], d=m[4], e=m[5], f=m[6], g=m[8], h=m[9], i=m[10];
	const double determinant = a*(e*i-f*h) - b*(d*i-f*g) + c*(d*h-e*g);
	if (!std::isfinite(determinant) || determinant == 0) return {};
	const std::array<double, 9> inverse {
	    (e*i-f*h)/determinant, (c*h-b*i)/determinant, (b*f-c*e)/determinant,
	    (f*g-d*i)/determinant, (a*i-c*g)/determinant, (c*d-a*f)/determinant,
	    (d*h-e*g)/determinant, (b*g-a*h)/determinant, (a*e-b*d)/determinant};
	for (unsigned row = 0; row < 3; ++row) {
		for (unsigned column = 0; column < 3; ++column)
			instance.object_to_world[row * 4 + column] = float(inverse[row * 3 + column]);
		instance.object_to_world[row * 4 + 3] = float(-(
		    inverse[row * 3] * m[3] + inverse[row * 3 + 1] * m[7] + inverse[row * 3 + 2] * m[11]));
	}
	for (float value : instance.object_to_world) if (!std::isfinite(value)) return {};
	// Conversion to float can collapse an otherwise invertible double matrix.
	const auto& n = instance.object_to_world;
	const double native_det = double(n[0])*(double(n[5])*n[10]-double(n[6])*n[9]) -
	    double(n[1])*(double(n[4])*n[10]-double(n[6])*n[8]) +
	    double(n[2])*(double(n[4])*n[9]-double(n[5])*n[8]);
	if (!std::isfinite(native_det) || native_det == 0) return {};
	return instance;
}
std::optional<float> AstroAccepts(uint32_t numerator, uint32_t denominator,
                                 uint32_t flags, float extent) {
	// The instruction word at 0x514 encodes count=8, offset=19 (decimal).
	if (numerator == 0x07f80000 || (!(flags & 4) && (numerator >> 31) != ((flags >> 3) & 1))) return {};
	const float distance = std::bit_cast<float>(numerator) * (1.0f / std::bit_cast<float>(denominator));
	if (!(distance <= extent)) return {};
	return distance;
}
std::optional<CapturedAstroScene> DecodeCapturedAstroScene(std::span<const uint32_t> record,
                                                          std::span<const uint32_t> scene, bool embedded) {
	namespace C=ShaderRecompiler::BvhCapture;
	if (record.size()!=C::RecordWords || scene.size()!=C::SceneWords ||
	    record[C::Pc]!=0x520 || record[C::Status]!=C::Resident || record[C::NodeWords]!=16 ||
	    record[C::NodeHigh]!=0 || (record[C::NodeLow]&7)>=4 ||
	    scene[C::SceneStatus]!=C::CompleteScene || scene[C::SceneVersion]!=1 || scene[C::ScenePc]!=0x520 ||
	    scene[C::BlasBytes]<96 || scene[C::BlasBytes]>1024 || (scene[C::BlasBytes]&3)) return {};
	for(unsigned i=0;i<4;i++)if(record[C::InstanceLow+i]!=scene[C::SceneInstanceLow+i])return {};
	std::array<std::byte,160> instance_bytes{};
	for(unsigned i=0;i<40;i++)for(unsigned j=0;j<4;j++)
		instance_bytes[i*4+j]=std::byte(scene[C::InstanceData+i]>>(j*8));
	const uint64_t instance_address=scene[C::SceneInstanceLow]|(uint64_t(scene[C::SceneInstanceHigh])<<32);
	auto instance=DecodeAstroInstance(instance_bytes,instance_address,embedded);
	if(!instance || (instance->flags&12)!=scene[C::SceneFlags])return {};
	BvhSnapshot snapshot;
	snapshot.address=scene[C::BlasLow]|(uint64_t(scene[C::BlasHigh])<<32);
	snapshot.root=instance->root;snapshot.format=BvhFormat::AstroLeafLists;
	if(instance->blas_address!=snapshot.address)return {};
	std::copy_n(record.begin()+C::Descriptor,4,snapshot.descriptor.begin());
	snapshot.bytes.resize(scene[C::BlasBytes]);
	for(size_t i=0;i<snapshot.bytes.size();i++)
		snapshot.bytes[i]=std::byte(scene[C::BlasData+i/4]>>((i%4)*8));
	const uint64_t node_offset=uint64_t(record[C::NodeLow]>>3)*64;
	if(node_offset>snapshot.bytes.size() || 64>snapshot.bytes.size()-node_offset)return {};
	for(unsigned i=0;i<16;i++)if(record[C::Data+i]!=scene[C::BlasData+node_offset/4+i])return {};
	auto converted=Convert(snapshot);
	if(!converted || std::none_of(converted.geometry.primitives.begin(),converted.geometry.primitives.end(),
	    [&](const Primitive& p) {return p.node==record[C::NodeLow] && p.leaf_id==record[C::LeafId];}))return {};
	return CapturedAstroScene{std::move(snapshot),*instance};
}
std::optional<CapturedAstroScene> ReadAstroCapture(const std::filesystem::path& nodes,
                                                const std::filesystem::path& scenes) {
	namespace C=ShaderRecompiler::BvhCapture;
	const auto read=[](const std::filesystem::path& path,size_t max_words)->std::vector<uint32_t> {
		std::ifstream file(path,std::ios::binary|std::ios::ate);
		const auto size=file.tellg();
		if(!file || size<256 || size>std::streamoff(max_words*4) || (size%4)!=0)return {};
		std::vector<uint8_t> bytes(size_t(size),0);file.seekg(0);
		if(!file.read(reinterpret_cast<char*>(bytes.data()),size))return {};
		std::vector<uint32_t> words(bytes.size()/4,0);
		for(size_t i=0;i<words.size();i++)for(unsigned j=0;j<4;j++)words[i]|=uint32_t(bytes[i*4+j])<<(8*j);
		return words;
	};
	auto a=read(nodes,C::HeaderWords+C::MaxScenes*C::RecordWords);
	auto b=read(scenes,C::HeaderWords+C::MaxScenes*C::SceneWords);
	if(a.empty() || b.empty() || a[C::FileMagic]!=C::Magic || a[C::FileVersion]!=C::Version ||
	   a[C::Stride]!=C::RecordWords || a[C::Enabled]!=1 || a[C::Capacity]>C::MaxRecords ||
	   !a[C::Count] || a[C::Count]>a[C::Capacity] || a[C::Count]>C::MaxScenes ||
	   a.size()!=C::HeaderWords+a[C::Count]*C::RecordWords ||
	   b[C::FileMagic]!=0x43535642 || b[C::FileVersion]!=1 || b[C::Stride]!=C::SceneWords ||
	   b[C::Capacity]!=C::MaxScenes || b[C::Count]!=a[C::Count] ||
	   b.size()!=C::HeaderWords+b[C::Count]*C::SceneWords)return {};
	for(auto field:{C::ShaderLow,C::ShaderHigh,C::TickLow,C::TickHigh,C::GroupsX,C::GroupsY,C::GroupsZ})
		if(a[field]!=b[field])return {};
	if((a[C::ShaderLow]|(uint64_t(a[C::ShaderHigh])<<32))!=C::AstroShader)return {};
	return DecodeCapturedAstroScene(std::span(a).subspan(C::HeaderWords,C::RecordWords),
	                              std::span(b).subspan(C::HeaderWords,C::SceneWords));
}
std::optional<std::array<uint32_t,512>> MakeAstroNativeBinding(const BvhSnapshot& source,uint64_t address) {
	namespace N=ShaderRecompiler::AstroNativeBinding;
	if(!address || source.format!=BvhFormat::AstroLeafLists || source.bytes.size()>1024 ||
	   (source.bytes.size()&3) || (source.descriptor[1]&0x7f800000))return {};
	auto converted=Convert(source);
	if(!converted || converted.geometry.topology.size()!=1 || converted.geometry.primitives.size()>N::MaxPrimitives)return {};
	const auto& root=converted.geometry.topology[0];
	if(root[0]!=source.root || std::count_if(root.begin()+1,root.end(),[](uint32_t node){return node!=~0u;})!=1)return {};
	// One list has a fixed guest order. A multi-box subtree's traversal order
	// depends on each ray, so a flattened index cannot settle its ties.
	for(unsigned i=1;i<5;i++)if(root[i]!=~0u && (root[i]&7)!=0)return {};
	std::array<uint32_t,N::Words> words{};
	words[N::Magic]=N::MagicValue;words[N::Version]=N::VersionValue;
	words[N::TlasLow]=uint32_t(address);words[N::TlasHigh]=uint32_t(address>>32);
	words[N::BaseLow]=uint32_t(source.address);words[N::BaseHigh]=uint32_t(source.address>>32);
	words[N::Root]=source.root;words[N::ByteCount]=uint32_t(source.bytes.size());
	words[N::PrimitiveCount]=uint32_t(converted.geometry.primitives.size());
	std::copy(source.descriptor.begin(),source.descriptor.end(),words.begin()+N::Descriptor);
	for(size_t i=0;i<source.bytes.size();i++)words[N::Data+i/4]|=uint32_t(std::to_integer<uint8_t>(source.bytes[i]))<<((i%4)*8);
	for(size_t i=0;i<converted.geometry.primitives.size();i++) {
		words[N::Primitives+i*2]=converted.geometry.primitives[i].node;
		words[N::Primitives+i*2+1]=converted.geometry.primitives[i].leaf_id;
	}
	return words;
}
}
