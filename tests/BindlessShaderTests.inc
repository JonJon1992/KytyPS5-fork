// Ghost of Yotei keeps each T# inside a 440-byte material record: offset key * 440 + 32
// (S_MUL_I32 by a literal), read by S_BUFFER_LOAD_DWORDX8. The bindless path tracks it with the
// record stride and the host reads one T# per key at that stride.
void TestBindlessStridedRecords() {
  using namespace ShaderRecompiler;
  using namespace ShaderRecompiler::IR;
  const uint32_t code[] = {
      EncodeVop1(0x02, 8, 256),            // GPU-selected key: first lane of v0
      EncodeSop2(0x26, 9, 8, 255), 440u,   // s_mul_i32 s9, s8, 440
      EncodeSmem0(0x0b, 32, 0), (9u << 25u) | 32u, // s_buffer_load_dwordx8 s[32:39], V#, s9 offset:32
      EncodeMimg0(0x20, 0xf), EncodeMimg1(8, 8, 1, 1), // S# from user data s[4:7]
      EncodeMubuf0(0x1c, 0), EncodeMubuf1(8, 3, 1),
      EncodeSopp(0x01),
  };
  std::array<uint32_t, 16> roots {};
  roots[0] = 0x10000; roots[2] = 440u * 16u; // V# of 16 records
  // roots[4..7]: an all-zero S#.
  roots[12] = 0x30000; roots[14] = 64;
  auto options = MakeCompileOptions(ShaderType::Compute);
  ShaderComputeInputInfo compute {};
  compute.threads_num[0]  = 64;
  compute.threads_num[1]  = 1;
  compute.threads_num[2]  = 1;
  compute.thread_ids_num  = 1;
  options.input_info.compute = &compute;
  options.user_data = roots;
  options.bindless_images = true;
  const auto saved_options = GetCodegenOptions();
  auto codegen = saved_options;
  codegen.srt_variant_reads = true;
  // Compute shaders take strided records only with KYTY_BINDLESS_STRIDED_COMPUTE.
  codegen.bindless_strided_compute = false;
  SetCodegenOptions(codegen);
  Check(TranslateProgram(code, options).skip_dispatch,
        "compute strided record T# was tracked without KYTY_BINDLESS_STRIDED_COMPUTE");
  codegen.bindless_strided_compute = true;
  SetCodegenOptions(codegen);
  auto translated = TranslateProgram(code, options);
  SetCodegenOptions(saved_options);
  Check(!translated.skip_dispatch && translated.program.info.images.size() == 1 &&
            translated.program.info.images[0].bindless,
        "strided record T# was not tracked as bindless");
  const auto& source =
      translated.program.descriptor_sources.at(translated.program.info.images[0].source);
  Check(source.indirect_image && source.indirect_image->record_stride == 440u &&
            source.indirect_image->table_offset == 32u,
        "strided record T# lost its stride or offset");
  auto plan = ExtractResourcePlan(translated.program);
  std::vector<uint8_t> bytes;
  ResourcePlan decoded;
  Check(EncodeResourcePlan(plan, bytes) && DecodeResourcePlan(bytes, decoded),
        "strided bindless plan codec failed");
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  // The host reads the records when it fills the heap, not during materialization: a reader
  // that refuses every read is enough.
  uint32_t heap_reads = 0;
  SrtRuntime runtime {.user_data = roots};
  runtime.userdata                   = &heap_reads;
  runtime.read_specialization_memory = [](void *reads, uint64_t, std::span<uint32_t>) {
    ++*static_cast<uint32_t *>(reads);
    return false;
  };
  const bool materialized = MaterializeResources(decoded, runtime, snapshot, specialization);
  Check(materialized && heap_reads == 0 && snapshot.bindless_heaps.size() == 1 &&
            snapshot.bindless_heaps[0].record_stride == 440u &&
            snapshot.bindless_heaps[0].table_offset == 32u,
        "strided bindless heap use lost its stride");
  auto result = CompileProgram(std::move(translated), options, specialization);
  CheckSpirvBinaryValidates(result.spirv);

  // Without bindless the stride stays unsupported: with KYTY_SRT_VARIANT_READS (the preset) the
  // shader is still skipped, not mistracked.
  options.bindless_images = false;
  const auto saved = GetCodegenOptions();
  auto variant_reads = saved;
  variant_reads.srt_variant_reads = true;
  SetCodegenOptions(variant_reads);
  const auto skipped = TranslateProgram(code, options);
  SetCodegenOptions(saved);
  Check(skipped.skip_dispatch, "strided record T# without bindless was not skipped");
}

// A depth-comparison sample is not served by the bindless arrays: with bindless images on, the
// handle keeps the CPU-enumerated path (or the shader is skipped) instead of becoming a bindless
// image whose draws ResourceMaterialization would drop ("bindless requires a non-comparison ...").
void TestBindlessCompareStaysOff() {
  using namespace ShaderRecompiler;
  const uint32_t code[] = {
      EncodeVop1(0x02, 8, 256),    // GPU-selected key: first lane of v0
      EncodeSop2(0x1e, 9, 8, 133), // image key * 32
      EncodeSmem0(0x0b, 32, 0), 9u << 25u,
      EncodeMimg0(0x28, 0x1), EncodeMimg1(8, 8, 1, 1), // image_sample_c, S# from s[4:7]
      EncodeMubuf0(0x1c, 0), EncodeMubuf1(8, 3, 1),
      EncodeSopp(0x01),
  };
  std::array<uint32_t, 16> roots {};
  roots[0] = 0x10000; roots[2] = 4096;
  roots[12] = 0x30000; roots[14] = 64;
  auto options = MakeCompileOptions(ShaderType::Compute);
  ShaderComputeInputInfo compute {};
  compute.threads_num[0] = 64;
  compute.threads_num[1] = 1;
  compute.threads_num[2] = 1;
  compute.thread_ids_num = 1;
  options.input_info.compute = &compute;
  options.user_data = roots;
  options.bindless_images = true;
  const auto saved = GetCodegenOptions();
  auto codegen = saved;
  codegen.srt_variant_reads = true;
  SetCodegenOptions(codegen);
  const auto translated = TranslateProgram(code, options);
  SetCodegenOptions(saved);
  Check(translated.skip_dispatch || translated.program.info.images.empty() ||
            !translated.program.info.images[0].bindless,
        "depth-comparison sample became a bindless image");
}

void TestBindlessShaderContract() {
  using namespace ShaderRecompiler;
  using namespace ShaderRecompiler::IR;
  const uint32_t code[] = {
      EncodeVop1(0x02, 8, 256), // GPU-selected key: first lane of v0
      EncodeSop2(0x1e, 9, 8, 133), // image key * 32
      EncodeSmem0(0x0b, 32, 0), 9u << 25u,
      EncodeSop2(0x1e, 9, 8, 132), // sampler key * 16
      EncodeSmem0(0x0a, 40, 2), 9u << 25u,
      EncodeMimg0(0x20, 0xf), EncodeMimg1(8, 8, 10, 1),
      EncodeMubuf0(0x1c, 0), EncodeMubuf1(8, 3, 1),
      EncodeSopp(0x01),
  };
  std::array<uint32_t, 16> roots {};
  roots[0] = 0x10000; roots[2] = 4096;
  roots[4] = 0x20000; roots[6] = 1024;
  roots[12] = 0x30000; roots[14] = 64;
  auto options = MakeCompileOptions(ShaderType::Compute);
  // v0 holds the thread id only with thread_ids_num; otherwise it is the constant 0 and its first
  // lane a static key. 64 threads keep the key GPU-selected.
  ShaderComputeInputInfo compute {};
  compute.threads_num[0]  = 64;
  compute.threads_num[1]  = 1;
  compute.threads_num[2]  = 1;
  compute.thread_ids_num  = 1;
  options.input_info.compute = &compute;
  options.user_data = roots;
  options.bindless_images = options.bindless_samplers = true;
  auto translated = TranslateProgram(code, options);
  Check(translated.program.info.images.size() == 1 && translated.program.info.images[0].bindless,
        "GPU-selected image was not tracked as bindless");
  Check(translated.program.info.samplers.size() == 1 && translated.program.info.samplers[0].bindless,
        "GPU-selected sampler was not tracked as bindless");
  Program cloned;
  Check(CloneProgram(translated.program, cloned) && cloned.bindless_images && cloned.bindless_samplers,
        "clone lost bindless policy");
  auto plan = ExtractResourcePlan(translated.program);
  std::vector<uint8_t> bytes;
  Check(EncodeResourcePlan(plan, bytes), "bindless plan encoding failed");
  ResourcePlan decoded;
  Check(DecodeResourcePlan(bytes, decoded), "bindless plan decoding failed");
  Check(decoded.bindless_images && decoded.bindless_samplers,
        "bindless policy did not survive codec");
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  // A bindless sampler plan requires a specialization reader (requires_specialization_memory):
  // one that refuses every read shows the materialization reads no guest heap.
  uint32_t heap_reads = 0;
  SrtRuntime runtime {.user_data = roots};
  runtime.userdata                   = &heap_reads;
  runtime.read_specialization_memory = [](void *reads, uint64_t, std::span<uint32_t>) {
    ++*static_cast<uint32_t *>(reads);
    return false;
  };
  Check(MaterializeResources(decoded, runtime, snapshot, specialization) && heap_reads == 0,
        "decoded bindless plan did not materialize without guest heap reads");
  Check(snapshot.bindless_heaps.size() == 1 && snapshot.bindless_sampler_heaps.size() == 1,
        "decoded bindless plan lost heap roots");
  bytes.clear();
  EncodeSpecialization(specialization, bytes);
  ResourceSpecialization decoded_specialization;
  Check(DecodeSpecialization(bytes, decoded_specialization) && decoded_specialization == specialization,
        "bindless specialization codec mismatch");
  auto result = CompileProgram(std::move(translated), options, specialization);
  const auto source = DisassembleSpirvBinary(result.spirv);
  Check(source.find("bindless_images") != std::string::npos &&
        source.find("bindless_samplers") != std::string::npos,
        "runtime descriptor arrays absent from SPIR-V");
  Check(source.find("OpAtomicOr") != std::string::npos,
        "bindless feedback did not use atomic OR");
  Check(SpirvHasDecorationValue(result.spirv, 34u, 1u), "bindless set1 declaration missing");
  CheckSpirvBinaryValidates(result.spirv);
  TestBindlessStridedRecords();
  TestBindlessCompareStaysOff();
}
