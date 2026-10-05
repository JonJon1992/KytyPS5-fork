// Regression coverage for instruction classification and memory address assembly.
void TestFrontendInstructionPrefixes() {
  using namespace ShaderRecompiler::Decoder;
  struct PrefixRange {
    uint32_t first;
    uint32_t last;
    Family family;
  };
  const PrefixRange ranges[] = {
      {0x000, 0x0f7, Family::VOP2}, {0x0f8, 0x0fb, Family::VOPC},
      {0x0fc, 0x0ff, Family::VOP1}, {0x100, 0x15f, Family::SOP2},
      {0x160, 0x17c, Family::SOPK}, {0x17d, 0x17d, Family::SOP1},
      {0x17e, 0x17e, Family::SOPC}, {0x17f, 0x17f, Family::SOPP},
      {0x190, 0x197, Family::VINTRP}, {0x198, 0x19f, Family::VOP3P},
      {0x1a8, 0x1af, Family::VOP3}, {0x1b0, 0x1b7, Family::DS},
      {0x1b8, 0x1bf, Family::FLAT}, {0x1c0, 0x1c7, Family::MUBUF},
      {0x1d0, 0x1d7, Family::MTBUF}, {0x1e0, 0x1e7, Family::MIMG},
      {0x1e8, 0x1ef, Family::SMEM}, {0x1f0, 0x1f7, Family::EXP},
  };
  for (uint32_t prefix = 0; prefix < 512; ++prefix) {
    auto expected = Family::Unknown;
    for (const auto& range : ranges) {
      if (prefix >= range.first && prefix <= range.last) expected = range.family;
    }
    for (const auto suffix : {0u, 0x13579bu, 0x7fffffu}) {
      Check(GetInstructionFamily((prefix << 23u) | suffix) == expected,
            "instruction family changed within a nine-bit encoding prefix");
    }
  }
}

void TestFrontendBufferAddresses() {
  namespace D = ShaderRecompiler::Decoder;
  namespace IR = ShaderRecompiler::IR;
  struct BufferCase {
    bool typed;
    uint32_t encoding;
    IR::ValueOpcode operation;
  };
  const BufferCase cases[] = {
      {false, 0x0c, IR::ValueOpcode::LoadBufferU32},
      {false, 0x1c, IR::ValueOpcode::StoreBufferU32},
      {false, 0x32, IR::ValueOpcode::BufferAtomicIAdd32},
      {true, 0x00, IR::ValueOpcode::LoadBufferU32},
      {true, 0x04, IR::ValueOpcode::StoreBufferU32},
  };
  for (const auto& item : cases) {
    for (uint32_t mode = 0; mode < 4; ++mode) {
      for (const auto soffset : {10u, 128u, 193u, 124u}) {
        const bool idxen = (mode & 1u) != 0;
        const bool offen = (mode & 2u) != 0;
        const uint32_t words[] = {
            item.typed ? EncodeMtbuf0(item.encoding, 4, 4, 31, idxen, offen)
                       : EncodeMubuf0(item.encoding, 31, idxen) | (offen ? 1u << 12u : 0u),
            EncodeMubuf1(20, 2, 5, soffset),
        };
        D::Instruction decoded;
        D::DecodeInstruction(words, 0, decoded);
        IR::Program program;
        IR::Block block;
        ShaderRecompiler::Frontend::Translator translator(program, &block, 256, false);
        translator.TranslateInstruction(decoded);
        const auto operation = std::find_if(block.begin(), block.end(), [&](const auto& inst) {
          return inst.GetOpcode() == item.operation;
        });
        Check(operation != block.end(), "buffer address fixture lost its memory operation");
        const auto vector_address = [&](IR::Value value, bool enabled, uint32_t reg) {
          if (!enabled) return value.IsImmediate() && value.U32() == 0u;
          const auto* read = value.TryInstruction();
          return read != nullptr && read->GetOpcode() == IR::ValueOpcode::GetVectorRegister &&
                 IR::RegIndex(read->Arg(0).VectorRegister()) == reg;
        };
        Check(vector_address(operation->Arg(1), idxen, 5u) &&
                  vector_address(operation->Arg(2), offen, idxen ? 6u : 5u),
              "buffer index/offset modes selected the wrong address VGPRs");
        const auto scalar_offset = operation->Arg(3);
        if (soffset == 128u || soffset == 193u) {
          Check(scalar_offset.IsImmediate() &&
                    scalar_offset.U32() == (soffset == 128u ? 0u : UINT32_MAX),
                "buffer scalar offset lost its inline constant");
        } else {
          const auto* read = scalar_offset.TryInstruction();
          Check(read != nullptr &&
                    read->GetOpcode() == (soffset == 124u ? IR::ValueOpcode::GetM0
                                                        : IR::ValueOpcode::GetScalarRegister),
                "buffer scalar offset selected the wrong scalar register kind");
          if (soffset == 10u) {
            Check(IR::RegIndex(read->Arg(0).ScalarRegister()) == 10u,
                  "buffer scalar offset selected the wrong SGPR");
          }
        }
      }
    }
  }
}

void TestFrontendImageAddresses() {
  namespace D = ShaderRecompiler::Decoder;
  namespace IR = ShaderRecompiler::IR;
  // Derivative samples exercise long addresses; stores also construct a data vector.
  for (const auto opcode : {0x22u, 0x08u}) {
    for (const auto a16 : {false, true}) {
      for (const auto nsa_dwords : {0u, 1u, 3u}) {
        const uint32_t words[] = {EncodeMimg0(opcode, 15, false, 3),
                                  EncodeMimg1(20, 2, 3, 5, a16)};
        D::Instruction decoded;
        D::DecodeInstruction(words, 0, decoded);
        decoded.image_nsa_dwords = nsa_dwords;
        for (uint32_t i = 0; i < D::MaxImageNsaAddressComponents; ++i) {
          decoded.image_nsa_addr[i] = 40 + 3 * i;
        }
        IR::Program program;
        IR::Block block;
        ShaderRecompiler::Frontend::Translator translator(program, &block, 256, false);
        translator.TranslateInstruction(decoded);
        const auto address = std::find_if(block.begin(), block.end(), [](const auto& inst) {
          return inst.GetOpcode() == IR::ValueOpcode::MakeImageAddress;
        });
        Check(address != block.end(), "image translation lost its address");
        const auto count = D::ImageAddressDwordCount(decoded.image_sample_flags,
                                                     decoded.image_address_components);
        for (uint32_t i = 0; i < address->NumArgs(); ++i) {
          const auto value = address->Arg(i);
          if (i >= count) {
            Check(value.IsImmediate() && value.U32() == 0u,
                  "unused image address components were not zero-filled");
            continue;
          }
          const auto reg = i != 0u && i - 1u < nsa_dwords * 4u
                             ? decoded.image_nsa_addr[i - 1u] : 5u + i;
          const auto* read = value.TryInstruction();
          Check(read != nullptr && read->GetOpcode() == IR::ValueOpcode::GetVectorRegister &&
                    IR::RegIndex(read->Arg(0).VectorRegister()) == reg,
                "image address changed its contiguous/NSA register order or A16 packing");
        }
        if (opcode == 0x08u) {
          const auto data = std::find_if(block.begin(), block.end(), [](const auto& inst) {
            return inst.GetOpcode() == IR::ValueOpcode::CompositeConstructU32x4;
          });
          Check(data != block.end(), "image store lost its data vector");
          for (uint32_t i = 0; i < 4; ++i) {
            const auto* read = data->Arg(i).TryInstruction();
            Check(read != nullptr && read->GetOpcode() == IR::ValueOpcode::GetVectorRegister &&
                      IR::RegIndex(read->Arg(0).VectorRegister()) == 20u + i,
                  "image store changed the order of its data registers");
          }
        }
      }
    }
  }
}
