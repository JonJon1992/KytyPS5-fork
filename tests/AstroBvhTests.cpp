#include "graphics/host_gpu/renderer/rt/astroBvh.h"
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "graphics/shader/recompiler/BvhCapture.h"
#include "graphics/shader/recompiler/AstroNativeBinding.h"
#include <fstream>
#include <chrono>
using namespace Libs::Graphics::RT;
void Check(bool ok, const char* message) {
	if (!ok) { std::fprintf(stderr,"FAIL: %s\n",message); std::abort(); }
}
int main() {
	std::array<std::byte,160> bytes {};
	const auto word = [&](size_t offset,uint32_t value) {
		for(unsigned i=0;i<4;i++)bytes[offset+i]=std::byte(value>>(8*i));
	};
	const auto value = [&](size_t offset,float f) { word(offset,std::bit_cast<uint32_t>(f)); };
	word(0,256|8);word(8,0x10000);word(152,37);
	value(16,2);value(36,3);value(56,-4);
	value(64,10);value(68,12);value(72,16);
	auto decoded=DecodeAstroInstance(bytes,0x20000);
	Check(bool(decoded),"decode resident 160-byte Astro instance");
	Check(decoded->blas_address==0x10000 && decoded->root==37 && decoded->flags==(256|8),"instance metadata");
	Check(decoded->world_to_object[0]==2 && decoded->world_to_object[5]==3 &&
	      decoded->world_to_object[10]==-4 && decoded->world_to_object[3]==10,"matrix follows shader's strided loads");
	Check(decoded->object_to_world[0]==.5f && std::abs(decoded->object_to_world[5]-1.f/3.f)<1e-7f &&
	      decoded->object_to_world[10]==-.25f && decoded->object_to_world[3]==-5 &&
	      decoded->object_to_world[7]==-4 && decoded->object_to_world[11]==4,"inverse affine transform with reflected axis");
	value(32,1);
	auto shear=DecodeAstroInstance(bytes,0x20000);
	Check(shear && shear->world_to_object[1]==1 && shear->world_to_object[4]==0 &&
	      std::abs(shear->object_to_world[1]+1.f/6.f)<1e-7f &&
	      shear->object_to_world[3]==-3,"off-diagonal loads and inverse translation");
	value(32,0);
	word(152,~0u);
	Check(!DecodeAstroInstance(bytes,0x20000),"unsupported instance root falls back");
	Check(DecodeAstroInstance(bytes,0x20000,true)->root==37,"embedded instance uses shader's default root");
	word(152,37);value(56,0);
	Check(!DecodeAstroInstance(bytes,0x20000),"singular transform falls back");
	value(56,-4);value(16,std::bit_cast<float>(0x7fc00000u));
	Check(!DecodeAstroInstance(bytes,0x20000),"NaN transform falls back");
	value(16,2);
	Check(!DecodeAstroInstance(std::span(bytes).first(159),0x20000),"incomplete instance falls back");
	Check(!DecodeAstroInstance(bytes,(1ull<<40)-80),"instance aperture overflow");
	word(0,255);Check(!DecodeAstroInstance(bytes,0x20000),"shader skips flags <=255");
	const auto fbits=[](float v){return std::bit_cast<uint32_t>(v);};
	Check(AstroAccepts(fbits(-8),fbits(-4),8,2)==2,"negative numerator selected at extent equality");
	Check(!AstroAccepts(fbits(-8),fbits(-4),0,10),"face sign rejected");
	Check(AstroAccepts(fbits(-8),fbits(-4),4,2)==2,"two-sided sign bypass");
	Check(!AstroAccepts(fbits(8),fbits(4),0,1),"distance rejected");
	Check(!AstroAccepts(0x7f800000,fbits(1),4,10),"software INF miss rejected by extent");
	Check(!AstroAccepts(0x07f80000,fbits(1),4,10),"actual S_BFM mask is 8 bits at decimal offset 19");
	Check(!AstroAccepts(0x7fc00000,fbits(1),4,10),"unordered comparison rejects NaN");
	Check(AstroAccepts(0x80000000,fbits(-1),8,0)==0,"negative-zero numerator sign preserved");
	namespace C=Libs::Graphics::ShaderRecompiler::BvhCapture;
	std::array<uint32_t,C::SceneWords> scene {};
	scene[C::SceneStatus]=C::CompleteScene;scene[C::SceneVersion]=1;scene[C::ScenePc]=0x520;
	scene[C::BlasBytes]=928;scene[C::BlasLow]=0x10000;scene[C::SceneInstanceLow]=0x20000;
	scene[C::SceneFlags]=8;scene[C::SceneLeafId]=0x80000049;
	word(0,256|8);
	for(unsigned i=0;i<40;i++)for(unsigned j=0;j<4;j++)
		scene[C::InstanceData+i]|=uint32_t(std::to_integer<uint8_t>(bytes[i*4+j]))<<(8*j);
	auto b=std::span(scene).subspan(C::BlasData);
	b[0]=0x5f525350;b[1]=0x4c485642;b[4]=928;b[8]=768;b[20]=12;b[22]=116;
	b[64]=832;for(unsigned i=1;i<4;i++)b[64+i]=~0u;
	b[208]=64;b[209]=0x80000049;
	b[128+3]=fbits(1);b[128+7]=fbits(1);
	std::array<uint32_t,C::RecordWords> record{};
	record[C::Pc]=0x520;record[C::Status]=C::Resident;record[C::NodeLow]=64;record[C::NodeWords]=16;
	record[C::Descriptor]=0x100;record[C::Descriptor+1]=0x80000000;
	record[C::Descriptor+2]=11;record[C::Descriptor+3]=0x81000000;
	record[C::InstanceLow]=0x20000;record[C::LeafId]=0x80000049;record[C::InstanceFlags]=8;
	for(unsigned i=0;i<16;i++)record[C::Data+i]=b[128+i];
	auto decoded_scene=DecodeCapturedAstroScene(record,scene);
	Check(decoded_scene && decoded_scene->bvh.format==BvhFormat::AstroLeafLists &&
	      decoded_scene->bvh.bytes.size()==928 && decoded_scene->instance.address==0x20000,
	      "completed GPU sidecar becomes checked Astro snapshot and instance");
	namespace N=Libs::Graphics::ShaderRecompiler::AstroNativeBinding;
	auto binding=MakeAstroNativeBinding(decoded_scene->bvh,0x123456789abcdef0ull);
	Check(binding && (*binding)[N::TlasLow]==0x9abcdef0 && (*binding)[N::TlasHigh]==0x12345678 &&
	      (*binding)[N::ByteCount]==928 && (*binding)[N::PrimitiveCount]==1 &&
	      (*binding)[N::Primitives]==64 && (*binding)[N::Primitives+1]==0x80000049,
	      "native binding preserves address, source bytes and raw signed ID");
	Check(!MakeAstroNativeBinding(decoded_scene->bvh,0),"null native address rejected");
	auto unsupported=decoded_scene->bvh;unsupported.format=BvhFormat::DirectNodes;
	Check(!MakeAstroNativeBinding(unsupported,1),"unproven source format rejected");
	const auto directory=std::filesystem::temp_directory_path()/
	    ("astro-scene-reader-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directory(directory);
	std::array<uint32_t,C::HeaderWords> header{};
	header[C::Count]=1;header[C::Enabled]=1;header[C::Capacity]=1;
	header[C::ShaderLow]=uint32_t(C::AstroShader);header[C::ShaderHigh]=uint32_t(C::AstroShader>>32);
	header[C::FileMagic]=C::Magic;header[C::FileVersion]=C::Version;header[C::Stride]=C::RecordWords;
	const auto save=[&](const char* name,std::span<const uint32_t> h,std::span<const uint32_t> payload) {
		std::ofstream file(directory/name,std::ios::binary);
		for(auto words:{h,payload})for(auto w:words)for(unsigned i=0;i<4;i++)file.put(char(w>>(8*i)));
	};
	save("nodes",header,record);
	header[C::FileMagic]=0x43535642;header[C::FileVersion]=1;header[C::Stride]=C::SceneWords;header[C::Capacity]=C::MaxScenes;
	save("scenes",header,scene);
	Check(bool(ReadAstroCapture(directory/"nodes",directory/"scenes")),"bounded paired capture reader");
	header[C::TickLow]=1;save("scenes",header,scene);
	Check(!ReadAstroCapture(directory/"nodes",directory/"scenes"),"different dispatch files rejected");
	header[C::TickLow]=0;header[C::Count]=0xffffffff;save("scenes",header,scene);
	Check(!ReadAstroCapture(directory/"nodes",directory/"scenes"),"overflowing scene count rejected");
	header[C::Count]=1;save("scenes",header,scene);
	std::filesystem::resize_file(directory/"scenes",300);
	Check(!ReadAstroCapture(directory/"nodes",directory/"scenes"),"truncated paired scene file rejected");
	std::filesystem::remove_all(directory);
	scene[C::SceneStatus]=C::MissingScene;
	Check(!DecodeCapturedAstroScene(record,scene),"missing GPU scene never becomes a snapshot");
	scene[C::SceneStatus]=C::CompleteScene;scene[C::SceneFlags]=4;
	Check(!DecodeCapturedAstroScene(record,scene),"shader flags must match captured instance");
	scene[C::SceneFlags]=8;scene[C::BlasBytes]=2048;
	Check(!DecodeCapturedAstroScene(record,scene),"oversized payload rejected");
	scene[C::BlasBytes]=928;scene[C::BlasLow]=0x11000;
	Check(!DecodeCapturedAstroScene(record,scene),"descriptor and instance must identify captured BLAS");
	Check(!DecodeCapturedAstroScene(record,std::span(scene).first(C::SceneWords-1)),"truncated scene slot rejected");
	std::puts("AstroBvhTests: instance layout, affine inverse, guards and shader acceptance passed");
}
