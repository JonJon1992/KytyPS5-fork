// Included by the standalone materialization target; no Vulkan device is needed.
Libs::Graphics::ShaderRecompiler::IR::ResourcePlan BindlessTestPlan() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = program.resource_tracking_complete = true;
  program.bindless_images = program.bindless_samplers = true;
  auto& block = AddValueBlock(program);
  DescriptorSource table;
  table.dword_count = 4;
  for (uint32_t i = 0; i < 4; ++i) {
    table.dwords[i] = Value(&block.AppendNewInst(ValueOpcode::GetUserData,
                              {Value(static_cast<ScalarReg>(i))}));
  }
  program.descriptor_sources.push_back(table);
  DescriptorSource image;
  image.dword_count = 8;
  image.indirect_image = DescriptorSource::IndirectImage {};
  image.indirect_image->table_source = 0;
  image.indirect_image->table_offset = 32;
  image.indirect_image->bindless = true;
  program.descriptor_sources.push_back(image);
  table.bindless_sampler = DescriptorSource::BindlessSampler {16};
  program.descriptor_sources.push_back(table);
  program.info.images.push_back({.source = 1, .resource_class = ImageResourceClass::Sampled,
      .dimension = Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension::Dim2D});
  program.info.samplers.push_back({.source = 2, .bindless = true});
  program.info.sampled_pairs.push_back({.image = 0, .sampler = 0});
  return ExtractResourcePlan(program);
}

void TestBindlessSnapshots() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = BindlessTestPlan();
  Check(plan.bindless_images && plan.bindless_samplers, "bindless policy lost from immutable plan");
  std::array<uint32_t, 4> roots {0x10000, 0, 4096, 0};
  ResourceSnapshot first, second;
  ResourceSpecialization a, b;
  uint32_t reads = 0;
  SrtRuntime runtime {.user_data = roots, .userdata = &reads,
      .read_specialization_memory = RejectSpecializationRead};
  Check(MaterializeResources(plan, runtime, first, a), "bindless roots failed to materialize");
  Check(reads == 0, "worker materialization tried to read the GPU-selected heap");
  Check(first.bindless_heaps.size() == 1 && first.bindless_sampler_heaps.size() == 1,
        "heap uses missing from snapshot");
  Check(first.bindless_heaps[0].base == roots[0] && first.bindless_heaps[0].size == roots[2],
        "heap base or byte extent was lost");
  Check(first.flattened_srt == std::vector<uint32_t>({0, 0, 0, 0}),
        "snapshot contains mutable host translation coordinates");
  roots[0] += 0x10000;
  roots[2] *= 2;
  Check(MaterializeResources(plan, runtime, second, b), "changed heap failed to materialize");
  Check(second.bindless_heaps[0].base != first.bindless_heaps[0].base &&
        second.bindless_heaps[0].size != first.bindless_heaps[0].size && a == b,
        "runtime heap roots polluted specialization or the previous snapshot");
  Check(!MaterializeResources(plan, {}, second, b), "missing clean root accepted");
  Check(second.bindless_heaps.empty() && second.bindless_sampler_heaps.empty(),
        "failed materialization retained stale heap applicability");
  plan.info.images[0].dimension =
      Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension::Dim2DMsaa;
  Check(!MaterializeResources(plan, runtime, second, b), "MSAA accepted into sampled bindless ABI");
  Check(second.bindless_heaps.empty(), "unsupported image retained heap applicability");
  plan.info.images[0].dimension =
      Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension::Dim2D;
  plan.info.images[0].written = true;
  Check(!MaterializeResources(plan, runtime, second, b), "writable bindless image accepted");
}
