// Full captured Astro BLAS, compared with the production software BVH opcode
// and the game's signed-ID list and facing/extent rules. Offline audit only.
namespace AstroSceneReplay {
namespace C = ShaderRecompiler::BvhCapture;
void StoreHookOutput(std::vector<u32>& code,u32 value,u32 lane,u32 offset) {
  // s48..51 are the harness's usual output descriptor, but the real Astro
  // loop owns s50/s51. Use the separate input/output descriptor at s0..3.
  AppendVMovU32(&code,31,offset);
  code.push_back(EncodeVop2(0x25,31,Vgpr(lane),31));
  code.push_back(EncodeVop2(0x1a,31,InlineU32(2),31));
  code.insert(code.end(),{EncodeMubuf0(0x1c),EncodeMubuf1(value,0,31)});
}
void CheckHookTranslation(VulkanHarness& vulkan,u32 wave=32) {
  auto options=ShaderRecompiler::GetCodegenOptions();options.astro_hardware_rt=true;
  CodegenTests::ScopedCodegenOptions scoped(options);
  TestCase test;test.name=wave==64?"AstroNativeLoopHookWave64":"AstroNativeLoopHookWave32";test.shader_hash=C::AstroShader;
  test.initial.resize(256);test.compute_info.wave_size=wave;
  test.compute_info.threads_num[0]=wave;test.compute_info.threads_num[1]=test.compute_info.threads_num[2]=1;
  test.compute_info.thread_ids_num=1;test.has_compute_info=true;
  AppendVMovLiteral(&test.code,23,0x41200000);
  while(test.code.size()*4<0x4a0)test.code.push_back(EncodeSopp(0));
  test.code.push_back(EncodeVop1(0x2a,24,Vgpr(15)));
  StoreHookOutput(test.code,23,0,0);AppendEnd(&test.code);
  test.ir_counts={{"AstroTrace",1}};
  (void)CompileCase(test,vulkan.SubgroupSize());
}
struct Entry { u32 record; RT::Ray ray; };
struct Group { RT::BvhSnapshot snapshot; u32 flags; std::vector<Entry> entries; };
struct Expected { u32 primitive = ~0u; float t = 0; };

std::vector<Expected> Software(VulkanHarness& vulkan, const Group& group) {
  constexpr const char* name = "AstroFullBlasSoftware";
  auto geometry = RT::Convert(group.snapshot);
  Require(name, "single leaf list", geometry && geometry.geometry.topology.size() == 1 &&
          std::count_if(geometry.geometry.topology[0].begin()+1, geometry.geometry.topology[0].end(),
                        [](u32 x) { return x != ~0u; }) == 1,
          "this replay oracle requires one box and one ordered leaf list");
  const auto& primitives = geometry.geometry.primitives;
  const u32 count = u32(group.entries.size());
  constexpr u32 Output = 1024, NodeStart = 8192;
  Require(name, "bounded rays", count && count <= 64 && primitives.size() <= 24,
          "replay batch exceeds bounded fixture");
  TestCase test;
  test.name = name;
  test.initial.resize(NodeStart + group.snapshot.bytes.size()/4);
  for (u32 i=0;i<group.snapshot.bytes.size();++i)
    test.initial[NodeStart+i/4] |= u32(std::to_integer<uint8_t>(group.snapshot.bytes[i])) << ((i%4)*8);
  for(u32 lane=0;lane<64;++lane) {
    const auto& ray=group.entries[std::min(lane,count-1)].ray;
    auto at=std::span(test.initial).subspan(lane*16,16);
    at[1]=std::bit_cast<u32>(ray.max_t);
    for(u32 axis=0;axis<3;++axis) {
      at[2+axis]=std::bit_cast<u32>(ray.origin[axis]);
      at[5+axis]=std::bit_cast<u32>(ray.direction[axis]);
      at[8+axis]=std::bit_cast<u32>(1.f/ray.direction[axis]);
    }
  }
  auto& code=test.code;
  AppendSMovLiteral(&code,8,0x100); // Rebase the immutable bytes for this offline dispatch.
  for(u32 i=1;i<4;++i) AppendSMovLiteral(&code,8+i,group.snapshot.descriptor[i]);
  code.push_back(EncodeVop2(0x1a,1,InlineU32(6),0));
  for(u32 at:{0u,4u,8u})
    code.insert(code.end(),{EncodeMubuf0(0x0e,at*4),EncodeMubuf1(4+at,0,1)});
  code.push_back(EncodeVop2(0x1a,2,InlineU32(2),0));
  for(u32 node=0;node<=primitives.size();++node) {
    AppendVMovLiteral(&code,4,node==0 ? group.snapshot.root : primitives[node-1].node);
    code.insert(code.end(),{EncodeMimg0(0xe6,15,0,false,0,true),EncodeMimg1(20,4,2)});
    for(u32 component=0;component<4;++component)
      AppendStoreVgprAtLaneDwordOffset(&code,20+component,2,Output+node*256+component);
  }
  AppendEnd(&code);
  test.bda_mappings={{0x10000,NodeStart*4,u32(group.snapshot.bytes.size())}};
  test.compute_info.wave_size=64;
  test.compute_info.threads_num[0]=64;
  test.compute_info.threads_num[1]=test.compute_info.threads_num[2]=1;
  test.compute_info.thread_ids_num=1; test.has_compute_info=true;
  auto compiled=CompileCase(test,vulkan.SubgroupSize());
  auto buffer=vulkan.CreateStorageBuffer(name,test.initial,test.initial.size(),true);
  vulkan.Dispatch(test,compiled,buffer);
  auto words=vulkan.ReadBuffer(name,buffer,test.initial.size());
  vulkan.DestroyBuffer(&buffer);
  std::vector<Expected> result(count);
  for(u32 lane=0;lane<count;++lane) {
    // The guest's box test is exclusive at the entry extent, while its leaf
    // update uses <=. Preserve both rather than treating BLAS as triangle soup.
    if(std::ranges::all_of(std::span(words).subspan(Output+lane*4,4),[](u32 x){return x==~0u;}))continue;
    float extent=group.entries[lane].ray.max_t;
    for(u32 primitive=0;primitive<primitives.size();++primitive) {
      const u32 at=Output+(primitive+1)*256+lane*4;
      auto t=RT::AstroAccepts(words[at],words[at+1],group.flags,extent);
      if(t) {extent=*t;result[lane]={primitive,*t};}
    }
  }
  return result;
}

void CheckHookGpu(VulkanHarness& vulkan,const char* nodes,const char* scenes,u32 wave=32) {
  constexpr const char* name="AstroNativeLoopGpu";
  auto options=ShaderRecompiler::GetCodegenOptions();options.astro_hardware_rt=true;
  CodegenTests::ScopedCodegenOptions scoped(options);
  auto source=RT::ReadAstroCapture(nodes,scenes);
  std::string error;auto witness=BvhCaptureReplay::Read(nodes,error);
  Require(name,"source",source && witness && witness->records.size()==64,"need 64 completed real Astro rays");
  Group group{source->bvh,source->instance.flags&12,{}};
  for(u32 i=0;i<64;i++) {
    const auto& r=witness->records[i];RT::Ray ray{};ray.max_t=std::bit_cast<float>(r[C::Extent]);
    Require(name,"same policy",r[C::InstanceFlags]==group.flags,"mixed facing policy needs separate batches");
    for(u32 a=0;a<3;a++){ray.origin[a]=std::bit_cast<float>(r[C::Origin+a]);ray.direction[a]=std::bit_cast<float>(r[C::Direction+a]);}
    group.entries.push_back({i,ray});
  }
  auto expected=Software(vulkan,group);
  auto renderer=vulkan.MakeRenderContext();auto& backend=renderer->GetHardwareRt();
  auto& scheduler=renderer->GetCommandScheduler();HW::Context registers;HW::UserConfig user;HW::Shader shaders;
  scheduler.Begin(registers,user,shaders);
  auto scene=backend.Prepare(source->bvh);Require(name,"native scene",bool(scene),"source BLAS rejected");
  auto binding=RT::MakeAstroNativeBinding(source->bvh,backend.SceneAddress(scene));
  Require(name,"native address",bool(binding),"native binding unavailable");scheduler.Finish();
  constexpr u32 Output=1024,NodeStart=8192;
  TestCase test;test.name=wave==64?"AstroNativeLoopGpuWave64":"AstroNativeLoopGpuWave32";test.shader_hash=C::AstroShader;
  test.initial.resize(NodeStart+source->bvh.bytes.size()/4);
  for(u32 i=0;i<source->bvh.bytes.size();++i)test.initial[NodeStart+i/4]|=u32(std::to_integer<uint8_t>(source->bvh.bytes[i]))<<((i%4)*8);
  for(u32 lane=0;lane<64;lane++) {
    const auto& ray=group.entries[lane].ray;
    for(u32 a=0;a<3;a++) {test.initial[lane*16+a]=std::bit_cast<u32>(ray.origin[a]);test.initial[lane*16+4+a]=std::bit_cast<u32>(ray.direction[a]);}
    test.initial[lane*16+3]=std::bit_cast<u32>(ray.max_t);
  }
  auto& code=test.code;
  for(u32 i=0;i<4;i++)AppendSMovLiteral(&code,24+i,source->bvh.descriptor[i]);
  AppendSMovLiteral(&code,51,source->bvh.root);AppendSMovLiteral(&code,50,5);AppendSMovLiteral(&code,16,5);
  AppendSMovLiteral(&code,19,group.flags);AppendSMovLiteral(&code,46,0x20000);AppendSMovLiteral(&code,47,4);
  code.push_back(EncodeVop2(0x1a,1,InlineU32(6),0));
  for(u32 at:{0u,4u})code.insert(code.end(),{EncodeMubuf0(0x0e,at*4),EncodeMubuf1(32+at,0,1)});
  constexpr std::array<u32,7> target{11,14,21,23,15,17,16};
  for(u32 i=0;i<7;i++)code.push_back(EncodeVop1(0x01,target[i],Vgpr(32+i)));
  for(u32 r:{7u,13u,4u,5u})AppendVMovLiteral(&code,r,~0u);
  while(code.size()*4<0x4a0)code.push_back(EncodeSopp(0));
  code.push_back(EncodeVop1(0x2a,24,Vgpr(15)));
  // The miniature fixture ends the BLAS region here. The real shader restores
  // its instance EXEC at 0x9c0 before the TLAS stack continues.
  code.push_back(EncodeSop1(0x04,126,193));
  code.push_back(EncodeVop2(0x1a,2,InlineU32(2),0));
  code.push_back(EncodeVop1(0x01,8,51));code.push_back(EncodeVop1(0x01,9,16));
  constexpr std::array<u32,7> output_regs{23,7,13,4,5,8,9};
  for(u32 i=0;i<7;i++)StoreHookOutput(code,output_regs[i],2,Output+i*256);
  AppendEnd(&code);
  test.compute_info.wave_size=wave;test.compute_info.threads_num[0]=64;
  test.compute_info.threads_num[1]=test.compute_info.threads_num[2]=1;
  test.compute_info.thread_ids_num=1;test.has_compute_info=true;
  test.bda_mappings={{source->bvh.address,NodeStart*4,u32(source->bvh.bytes.size())}};
  test.astro_native_binding.assign(binding->begin(),binding->end());
  test.ir_counts={{"AstroTrace",1}};
  auto compiled=CompileCase(test,vulkan.SubgroupSize());
  const auto run=[&](const char* label,bool native) {
    auto buffer=vulkan.CreateStorageBuffer(name,test.initial,test.initial.size(),true);
    vulkan.Dispatch(test,compiled,buffer);
    auto words=vulkan.ReadBuffer(name,buffer,test.initial.size());vulkan.DestroyBuffer(&buffer);
    const auto primitives=backend.Primitives(scene);u32 hits=0;
    for(u32 lane=0;lane<64;lane++) {
      const bool hit=native && expected[lane].primitive!=~0u;hits+=hit;
      const auto p=hit?primitives[expected[lane].primitive]:RT::Primitive{~0u,0,~0u};
      Require(name,label,words[Output+256+lane*4]==p.node && words[Output+512+lane*4]==p.leaf_id,
              "wrong live guest node/ID at lane "+std::to_string(lane));
      const auto t=std::bit_cast<float>(words[Output+lane*4]);
      const auto reference=hit?expected[lane].t:group.entries[lane].ray.max_t;
      Require(name,label,std::abs(t-reference)<=2e-5f*std::max(1.f,std::abs(reference)),"wrong live extent");
      Require(name,label,words[Output+768+lane*4]==(hit?0x20000u:~0u) &&
              words[Output+1024+lane*4]==(hit?4u:~0u),"wrong live instance pointer");
      bool all_native=native;
      for(u32 k=(lane/32)*32;k<(lane/32+1)*32;k++)all_native&=expected[k].primitive!=~0u;
      Require(name,label,words[Output+1280+lane*4]==(all_native?~0u:source->bvh.root) &&
              words[Output+1536+lane*4]==5,"wrong wave's BLAS exit/stack state");
    }
    std::printf("AstroNativeLoopGpu wave%u: %s native_hits=%u\n",wave,label,hits);
  };
  run("real captured scene",true);
  test.initial[NodeStart+232-1]^=1;run("changed live list fallback",false);
  test.initial[NodeStart+232-1]^=1;
  test.bda_mappings[0].guest_base+=0x10000;run("missing current pages fallback",false);
  test.bda_mappings[0].guest_base-=0x10000;
  test.astro_native_binding.clear();run("unavailable scene fallback",false);
  // Twelve coincident leaves: the native candidate order must not decide the
  // game's raw signed ID. Its <= rule chooses the final list entry.
  auto duplicate=source->bvh;
  for(u32 i=0;i<12;i++)for(u32 j=0;j<4;j++)duplicate.bytes[832+i*8+j]=std::byte(64u>>(j*8));
  group.snapshot=duplicate;
  for(auto& e:group.entries)e.ray={{2,.2f,-.1f},0,{-1,0,0},10};
  expected=Software(vulkan,group);
  scene=backend.Prepare(duplicate);Require(name,"duplicate scene",bool(scene),"coincident leaves rejected");
  binding=RT::MakeAstroNativeBinding(duplicate,backend.SceneAddress(scene));scheduler.Finish();
  test.astro_native_binding.assign(binding->begin(),binding->end());
  for(u32 i=0;i<duplicate.bytes.size()/4;i++) {
    test.initial[NodeStart+i]=0;
    for(u32 j=0;j<4;j++)test.initial[NodeStart+i]|=u32(std::to_integer<uint8_t>(duplicate.bytes[i*4+j]))<<(j*8);
  }
  for(u32 lane=0;lane<64;lane++) {
    auto& ray=group.entries[lane].ray;
    for(u32 a=0;a<3;a++){test.initial[lane*16+a]=std::bit_cast<u32>(ray.origin[a]);test.initial[lane*16+4+a]=std::bit_cast<u32>(ray.direction[a]);}
    test.initial[lane*16+3]=std::bit_cast<u32>(ray.max_t);
    Require(name,"last signed ID",expected[lane].primitive==11,"software did not select final coincident leaf");
  }
  run("equal-distance signed-ID order",true);
  scheduler.Finish();scene.reset();backend.Clear();
}

int Run(VulkanHarness& vulkan,const char* main_path,const char* scene_path,const char* csv_path) {
  constexpr const char* name="AstroSceneReplay";
  std::string error;
  auto witness=BvhCaptureReplay::Read(main_path,error);
  if(!witness){std::fprintf(stderr,"%s: %s\n",name,error.c_str());return 2;}
  std::ifstream file(scene_path,std::ios::binary|std::ios::ate);
  const auto bytes=file.tellg();
  if(!file || bytes < C::HeaderWords*4 || bytes > (C::HeaderWords+C::MaxScenes*C::SceneWords)*4)return 2;
  std::vector<u32> words(size_t(bytes)/4);
  if((bytes%4)!=0)return 2;
  file.seekg(0);std::vector<uint8_t> raw(size_t(bytes),0);
  if(!file.read(reinterpret_cast<char*>(raw.data()),bytes))return 2;
  for(size_t i=0;i<words.size();++i)for(u32 j=0;j<4;++j)words[i]|=u32(raw[i*4+j])<<(j*8);
  if(words[C::FileMagic]!=0x43535642 || words[C::FileVersion]!=1 || words[C::Stride]!=C::SceneWords ||
     words[C::Count]>C::MaxScenes || words.size()!=C::HeaderWords+words[C::Count]*C::SceneWords ||
     words[C::Count]!=witness->records.size())return 2;
  for(u32 key:{C::ShaderLow,C::ShaderHigh,C::TickLow,C::TickHigh,C::GroupsX,C::GroupsY,C::GroupsZ})
    if(words[key]!=witness->header[key])return 2;
  if((uint64_t(words[C::ShaderHigh])<<32|words[C::ShaderLow])!=C::AstroShader)return 2;
  std::vector<Group> groups;
  for(u32 i=0;i<witness->records.size();++i) {
    const auto& r=witness->records[i];
    auto scene=RT::DecodeCapturedAstroScene(r,std::span(words).subspan(C::HeaderWords+i*C::SceneWords,C::SceneWords));
    Require(name,"complete scene",bool(scene),"invalid or incomplete scene at slot "+std::to_string(i));
    RT::Ray ray{};ray.max_t=std::bit_cast<float>(r[C::Extent]);
    for(u32 axis=0;axis<3;++axis) {
      ray.origin[axis]=std::bit_cast<float>(r[C::Origin+axis]);
      ray.direction[axis]=std::bit_cast<float>(r[C::Direction+axis]);
    }
    const u32 flags=scene->instance.flags&12;
    auto g=std::find_if(groups.begin(),groups.end(),[&](const Group& x){return x.snapshot==scene->bvh && x.flags==flags;});
    if(g==groups.end()){groups.push_back({std::move(scene->bvh),flags,{}});g=groups.end()-1;}
    g->entries.push_back({i,ray});
  }
  std::ofstream csv(csv_path);
  if(!csv)return 2;
  csv<<"record,software_node,hardware_node,software_leaf_id,hardware_leaf_id,software_t,hardware_t,hit_match,id_match,distance_match\n"<<std::setprecision(9);
  auto renderer=vulkan.MakeRenderContext();auto& backend=renderer->GetHardwareRt();
  auto& scheduler=renderer->GetCommandScheduler();
  HW::Context registers;HW::UserConfig user;HW::Shader shaders;
  scheduler.Begin(registers,user,shaders);
  u32 queries=0,hit_errors=0,id_errors=0,distance_errors=0;
  for(const auto& group:groups) {
    // Native and software use the very same immutable full BLAS, including
    // captured list order and signed final entry. No live CPU backing is read.
    auto expected=Software(vulkan,group);
    auto scene=backend.Prepare(group.snapshot);
    Require(name,"native scene",bool(scene),"full captured BLAS rejected");
    std::vector<RT::Ray> rays;for(const auto& e:group.entries)rays.push_back(e.ray);
    auto query=backend.Trace(scene,rays,{RT::RayPolicy::Mode::AstroFacing,group.flags});
    Require(name,"native rays",bool(query),"captured rays rejected");
    scheduler.Finish();auto hits=backend.Read(query);auto primitives=backend.Primitives(scene);
    Require(name,"completion",hits.size()==expected.size(),"unfinished query");
    for(u32 i=0;i<hits.size();++i) {
      bool sw=expected[i].primitive!=~0u,hw=hits[i].primitive!=~0u;
      const u32 sn=sw?primitives[expected[i].primitive].node:~0u;
      const u32 hn=hw?primitives[hits[i].primitive].node:~0u;
      const u32 si=sw?primitives[expected[i].primitive].leaf_id:~0u;
      const u32 hi=hw?primitives[hits[i].primitive].leaf_id:~0u;
      const bool hm=sw==hw,im=sn==hn && si==hi;
      const bool tm=!sw || !hw || std::abs(expected[i].t-hits[i].t)<=2e-5f*std::max(1.f,std::abs(expected[i].t));
      ++queries;hit_errors+=!hm;id_errors+=!im;distance_errors+=!tm;
      csv<<group.entries[i].record<<','<<sn<<','<<hn<<','<<si<<','<<hi<<','<<expected[i].t<<','<<hits[i].t<<','<<hm<<','<<im<<','<<tm<<'\n';
    }
  }
  scheduler.Finish();backend.Clear();csv.close();if(!csv)return 2;
  std::printf("AstroSceneReplay: complete=%u groups=%zu hit_errors=%u id_errors=%u distance_errors=%u\n",queries,groups.size(),hit_errors,id_errors,distance_errors);
  if(!queries)return 77;
  return (hit_errors || id_errors || distance_errors)?1:0;
}
} // namespace AstroSceneReplay
