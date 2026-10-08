# O que aproveitar do fork Xx-LiDAF-xX/KytyPS5 na guest-sync-release-mem

Data: 2026-10-07. Base de comparação: `guest-sync-release-mem` em `a6037785`. Portado até agora: só o par SGPR das máscaras (seção 4, primeira linha).

**Regra do usuário:** as nossas otimizações ficam. Nada pode substituir ou reverter:

- a sincronização BDA;
- o CP sequencer e o recorder;
- os certificados do DrawPrep e os draw runs;
- a velocidade de compilação de shader;
- os loads de storage buffer sem branch;
- os strides em runtime e os bindless;
- a chave do program cache.

## 1. O fork

- **Repositório:** `https://github.com/Xx-LiDAF-xX/KytyPS5`, branch `main`, topo `ab79dcfa` (2026-10-07).
- **No repositório local:**
  - o `main` está em `refs/lidaf/main`;
  - a tag `BryKytyPS5-2026-10-01-719050e` está em `refs/lidaf/bry-719050e`.
- **De que é feito o `main`:**
  1. o BryKytyPS5 do BryanKAdams em `719050e2`, que **já analisamos nesse mesmo commit** em 2026-10-03 (`docs/DIVISAO-TRAVAMENTOS.md`, seção "Análise do BryKytyPS5");
  2. o upstream até `0e8ded37` (2026-10-03), que é a base comum e já está na nossa branch;
  3. **21 commits novos**, de XxOtakuXx e Xx-LiDAF-xX, entre 2026-10-04 e 2026-10-07. Este documento cobre só esses 21. Para listá-los: `git log --no-merges refs/lidaf/main ^HEAD ^719050e2`.
- **Por que o `git cherry` engana:** ele aponta 239 commits ausentes, mas 230 são do Bryan.
- **Foco dos commits novos:** Windows, com RTX 4060 Ti e 5800X. O trabalho gira em torno de:
  - presets de desempenho e OSD;
  - DirectStorage;
  - um ajuste para desligar ray tracing;
  - o crash do Astro Bot (leitura de 8 bytes em `0x30`, exit 321), que o próprio fork admite não ter resolvido.
- **Os commits não são independentes.** Vários desfazem uns aos outros (`48ab05dd`, `f7fc7520`, `64773089`, `2c5047fe`, `b255bc6d`, `a7e9f3ca`, `489c83df`): estão consertando um merge ruim do próprio fork. Por isso o efeito foi avaliado no líquido, com `git diff 2233378f refs/lidaf/main` mais `git diff 719050e2 c8cf1492`.

**Como foi verificado:**

- `git merge-tree` simulou cada cherry-pick sobre o `HEAD`. Dos 239, só 14 entram limpos, e entre os 21 novos nenhum. Esses 14 são docs, testes e utilitários do Bryan, ou commits pequenos do upstream que não estão no Bry.
- Três revisões separadas, só de leitura, cobriram:
  - shader/SPIR-V;
  - runtime, memória, kernel e libs;
  - config, presets, OSD, launcher e o que sobrou da análise do Bry.
- Os pontos centrais foram conferidos de novo no `HEAD`: a máscara SGPR, o suporte a BVH e o preset padrão do launcher.

## 2. Alerta: os presets mudam a emulação sem avisar

O launcher do fork usa `performance_profile = 2` por padrão (`configuration.h:124`), o que passa `--preset balanced` em **todo lançamento** (`mainDialog.cpp:254`). Além disso, a cada salvamento ele regrava o `kyty_settings.ini` com `async-pipelines`, `relaxed-readback`, `pipeline-libraries` e `speculative-draws` ligados. Nada disso usa variável `KYTY_*`, então não aparece num A/B por ambiente.

| Ajuste | Quality | Balanced | Performance | O que faz | Nosso padrão |
| --- | --- | --- | --- | --- | --- |
| `gpu_timestamp_scale` | 100 | 115 | 130 | Escala os timestamps do guest para enganar a resolução dinâmica | não existe |
| `async_pipelines` | on (desde `7db02208`) | on | on | **Pula draws** enquanto o pipeline compila | `KYTY_ASYNC_PIPELINES=0` (`pipelineCache.cpp:4161`) |
| `relaxed_readback` | off | on | on | **Devolve bytes antigos** no readback | sem equivalente; o side copy espera (`bufferCache.h:81`) |
| `pipeline_libraries` (GPL) | on | on | on | GPL | `KYTY_PIPELINE_LIBRARY` off, porque tem regressão no Crash |
| `amd_cpu_enabled` | on | on | on | Patch de instruções e trampolim de 8 MB | false, só para Intel (`emulatorConfig.h:69`). No fork a lógica está invertida e ligaria na 5700X |
| Auto-optimize | — | — | — | Força aniso 16 e **desliga `MeshRestartSplit`** | não existe; a correção de primitive restart fica ligada |

**Não portar.** Todos trocam exatidão por FPS sem aviso. Os ajustes de motion blur, DoF, bloom, AO e res-scale são só guardados, e nenhum código os lê.

## 3. Já temos (não portar)

| LiDAF | O que é | Onde já está |
| --- | --- | --- |
| `c3a9c390` | SuspendPoint com ticket promise/future | Corrige dois bugs que o próprio fork criou: `release()` sem `acquire` em `48ab05dd` e o `WaitForIdle` de `1d3e48ca`. Nós usamos o done limitado (`KYTY_AGC_DONE_MODE=bounded`), em `graphicsRun.cpp:481-550`, mais o frame fence em `:370-395`. Portar mexeria no CP sequencer |
| `c3a9c390` | Ballot/ReadFirstLane da metade 1 reutilizam a metade 0 no wave64 | `spirvEmitterProgram.cpp:539-545` (upstream `f29bd0fe`) |
| `c3a9c390` | Clear de mip único sobre imagem cacheada multi-mip | `textureCache.cpp:3128-3130` (upstream `0ec3655f`) |
| `78e776e9` | V_CMP_NE_I64 (VOPC 0xa5) | `VectorAluOps.cpp:267`, `Compare.cpp:34`, `Translate.cpp:951` |
| `78e776e9` | Swapchain espera o tick da submissão anterior antes de reusar o semáforo de acquire | `swapchain.cpp:674-677` e `1151-1153` (`m_frame_ticks`) |
| `08a3215e` | S_BUFFER_LOAD com V# escolhido na GPU, lido via BDA | `LoadIndirectScalarBuffer` (`spirvEmitterMemory.cpp:1573-1613`). O nosso alinha a soma `(base+imm+soffset)&~3`, como manda o ISA. **Não importar:** o merge "limpo" deixaria um branch `IndirectBuffer` duplicado e morto |
| `08a3215e` | Fim do fallback de GetHandle que aceitava image/sampler inválido | Nunca tivemos esse fallback (`ResourceTracking.cpp:1437-1466`) |
| `08a3215e` | LaneMaskProjection wave32 | `ConstantPropagation.cpp:1060`, `:1458` (upstream `35759e48`) |
| `08a3215e` | Identidade do shader pelo hash do código (`hash=0`) | `ShaderMapEntry` com `code_hash`/`registered_hash` (`shader.cpp:71-128`) |
| `08a3215e` | Estado dos gatilhos (`TriggerEffectState`/`TriggerTravel`) | `a6d70665`, `bbe1f24b`, `6a8a89ee` (`controller.cpp:367/811/848`) |
| `ef6a730a`, `08a3215e`, `1d3e48ca` | Leitores SRT devolvem zero no ponteiro nulo (0x30/0x28) | `ReadRawWord` (`SrtWalker.cpp:1382-1420`) e `ReadShaderGuestMemory` (`pipelineCache.cpp:521-536`) zeram a página nula ou não mapeada. O fork zera também páginas GPU-dirty e o leitor da especulação, o que gera especialização errada |
| `7db02208` | MasterSemaphore loga VkResult e ticks | `masterSemaphore.cpp:53-57` e `138-142`, com DeviceLostReport |
| `de3e635b` | OSD com FPS e progresso do shader cache | Painel no F2 (`window.cpp:527`, `performanceOverlay.cpp:440`) |
| `de3e635b`, `c8cf1492` | `SDL_OpenGamepad` nulo; resize 0×0 | `window.cpp:313-327` e `373-377` |
| `489c83df` | Rename para `TryReadSparseBacking` | `memory.h:187`. É só conserto de link do fork |

## 4. Vale portar, sem mexer nas nossas otimizações

| LiDAF | O que corrige | Como portar | Risco |
| --- | --- | --- | --- |
| `08a3215e` (`Translate.cpp`) — **portado** | **Par SGPR das máscaras no wave64.** `ReadMask`, `ReadMaskValid` e `WriteMask` passam a usar `reg & ~1u`. Caso do fork: no CS `280e6a01e6b640f7` do Astro Bot, um V_CMP com `sdst=3` escrevia `s[3:4]` e corrompia o T# em `s4` | O hunk de `Translate.cpp` entra limpo. Dá para trazer junto `tests/ScalarMaskPairCases.inc`, mas o caso precisa ser registrado à mão | **Baixo**. Não muda nada com registrador par, e o LLVM decodifica da mesma forma (`Val>>1`). Antes de portar, conferir se aquele dispatch é mesmo wave64 (`shader.cpp:1247`): se for wave32, a correção esconde uma detecção errada. Ela também é parcial, porque `ReadU32Pair` (`Translate.cpp:507`) não alinha as outras fontes SGPR de 64 bits. Muda a versão do program cache |
| `78e776e9` | **IMAGE_BVH64_INTERSECT_RAY (0xe7):** 12 componentes, node pointer de 64 bits | `spirvEmitterBvh.cpp`, `Memory.cpp:1156` e `ShaderDecoder.cpp` entram limpos; `ImageOps.cpp:249/278` e `ShaderRecompiler.cpp` precisam de porte manual | **Médio**. Só vale se algum jogo nosso registrar `unsupported BVH intersection form ... opcode=0xe7`. Hoje esse dispatch é pulado (`ShaderRecompiler.cpp:772-793`). Não existe teste de GPU para BVH64. `ShaderRayTracingTests.inc:238` teria de mudar. A SPIR-V de todo shader com BVH muda. Muda a versão do program cache |
| `08a3215e` (`controller.cpp`) | O gatilho não reenvia um efeito igual ao já entregue. O cache é invalidado quando `SDL_SendGamepadEffect` falha. A busca do gamepad e o envio ficam dentro de `SDL_LockJoysticks`, o que protege no hotplug | Manual, em `SetTriggerEffect` (`controller.cpp:811-846`) e `SendTriggerEffect` (~`:866-896`) | Baixo. Opcional: o fork não mediu ganho |
| `c3a9c390` (`vma.cpp`) | Na falha de `vmaCreateImage`, logar VkResult, usage, flags e samples | 1 `LOGF` em `vma.cpp:809-811`. O chamador (`image.cpp:1011`) já loga extent e formato | Mínimo. Só diagnóstico |

## 5. Não portar

| LiDAF | O que é | Por quê |
| --- | --- | --- |
| `ef6a730a`, `08a3215e` | `GpuControlledBlocks` / `gpu_execution_only`: tudo depois de um branch varying fica fora do plano SRT | Em PS isso tira do planejamento a maior parte do shader. Resultado: mais loads BDA e `IndirectBuffer`, menos slots flat, o que pesa em BDA, bindless e strides. O próprio fork não resolveu o Astro com isso, e o nosso zero na página nula já cobre o sintoma |
| `ef6a730a` | Mesh fast_launch: `group = WorkgroupId + draw(6)` | Não temos fast_launch, e o caminho split já soma `MeshFirstGroupDword` (`Translate.cpp:1238-1243`) |
| `08a3215e` | ResourceMaterialization com lista finita de `sources` | Outro desenho. A nossa prova de sobreposição é inline (`ResourceMaterialization.cpp:1195-1218`, `1404`) |
| `78e776e9` | SrtWalker: `m_active_reads` por bloco e short-circuit em LogicalAnd/Or | Depende de `ResourceBlock::srt_reads`, que não temos. Mexe no walker que alimenta os certificados do DrawPrep, sem evidência de que precisamos |
| `1d3e48ca` | Remove o atalho HardwareStorageBufferBounds | É recurso do Bry que não temos. Os nossos loads sem branch não mudam |
| `c8cf1492`, `78e776e9`, `7db02208` | RT desligado por padrão: `ray_tracing_enabled` na chave do programa, `has_bvh` pula shaders, mods em `patches/rt-native/*.json` | Mexe na chave do program cache. Os mods estão todos `enabled:false` |
| `7db02208` | Worker APR/AMM assíncrono com prioridades | No fork ele substitui o executor do Bry, em que WaitOnAddress não fazia nada. O nosso executor síncrono já espera, com limite (`libAmpr.cpp:1386`, `1550-1575`). O worker do upstream (`c045f089`/`5bf9218b`/`da9b38af`) foi pulado de propósito em `eb313d6f`, e portar conflita com `cd18c9cb` e o counter bank. Só rever, a partir do upstream, se algum log mostrar `AMPR wait-on-address not satisfied` |
| `c8cf1492`, `48ab05dd`, `1d3e48ca` | `ApplyAutoFixes` por title ID: bypass de GI/RT no Astro Bot; 11 trocas `cmove`→`cmovbe` no Playroom (PPSA01325) | Altera o jogo sem opt-in e sem prova. Dos anchors do Playroom, 6 vêm logo depois de `test` (CF=0, não fazem nada) e 2 depois de `cmp` (mudam a semântica). O `LoadPlan` do fork também ficou permissivo: não confere o tamanho off/on e aceita versão por prefixo |
| `b2397625` | CLI aplica sozinho `<TITLE>.json` achado em cwd, `_Patches` ou app0, e aceita o caminho do jogo como argumento posicional | Contaminaria os A/B na CLI (preset U59). Com o nosso EXIT na falha de patch (`runtimeLinker.cpp:1469`), um JSON incompatível achado sozinho abortaria o boot. O argumento posicional é opcional |
| `7db02208` | Log: o padrão passa de Silent para File, `RedactPrivateInfo` com `std::regex` em toda linha | Apaga quase todo caminho no Linux, custa CPU por linha e aloca dentro do handler de falha |
| `1d3e48ca` | Latência de áudio de 40 para 60 ms | Sem evidência, e são +20 ms |
| `78e776e9` | NullTextureDesc em D32 para `depth_compare` | Sem evidência. No RADV é só aviso de validação |
| `c8cf1492` | vblank 0 = sem limite | Período 0 deixa o PresentThread em busy-loop. O nosso clampa (`videoOut.cpp:880`) |
| `c8cf1492` | Aniso forçado no samplerCache | Muda a chave do sampler, o que interage com o bindless. Só faria sentido como opt-in |
| `c8cf1492` | PlayGo trata todo chunk como válido | Já temos `playgo_hack` opt-in (`libPlayGo.cpp:56-62`) |
| `78e776e9`, `7db02208`, `1b618447`, `48ab05dd` | DirectStorage, `find_if` em GuestRange (bug do STL do MSVC), `-fmacro-prefix-map` e `PDBALTPATH` | Só Windows, ou só para binário distribuído |
| `78e776e9` | `tools/gpu_asset_streamer`, `universal_processor_router`, `universal_shader_cache`, `run-*.ps1` | Ferramentas soltas, sem integração. O README diz "no speedup claimed", e o shader cache pula draws |

## 6. Medidas do fork (Windows, RTX 4060 Ti, 5800X, código do Bry)

Valem pouco para Crash 4 e Yotei no Linux, mas indicam onde olhar:

- **Playroom, travadas** (as parcelas se sobrepõem e não somam):
  - de 4791 ms: pipeline-create 1619 ms, lookahead 2540 ms;
  - de 2740 ms: pipeline-create 1914 ms, lookahead 897 ms.
- **Playroom, DCC:** 159 drenagens completas de `DISPATCH_INDIRECT` somam 564 ms, cerca de 3,5 ms por frame. Por frame, a GPU fica ocupada 5,2 ms contra um gap de 11,5 ms. Pista para nós: medir os motivos de fallback do DCC no Playroom, porque já temos os contadores (`profiler.h:325`).
- **Astro Bot, gameplay:**
  - a GPU fica ocupada 25,6 ms contra um gap de 46,0 ms por frame;
  - o principal ponto de write-fault tem 92.217 faltas (1,2 s) numa thread tbb;
  - a compilação de shader custa só 1,1 ms.

## 7. Situação dos candidatos da análise do BryKytyPS5 (2026-10-03)

| Candidato | Situação no `HEAD` |
| --- | --- |
| Wave32 em subgroup de 64 lanes (`8f02ad19` + `067e8826`) | Portado em `b2f18ba6` (`KYTY_DEBUG_WAVE_HALVES=0` desfaz). Ainda não foi validado em jogo |
| Bounds de storage na AMD com alinhamento 4 | Portado só como piloto em `fc3bf396`: `KYTY_AMD_BUFFER_BOUNDS`, off, fora do U59 |
| `2e8eae14`: textura atualizada pela CPU várias vezes vira buffer | **Não portado**. `ObtainBufferForImage` (`bufferCache.cpp:3835`) ainda faz staging toda vez |
| Prefetch de pipelines | Equivalente monolítico em `c02c777c` (`KYTY_PIPELINE_PREFETCH`, ligado no U59) |
| `26d62f80`: color attachment pelo write mask | Não portado. Só importa com `KYTY_SKIP_INACTIVE_PS=1`, que está off |
| GPL | Opt-in (`KYTY_PIPELINE_LIBRARY`), off por causa da regressão no Crash |
| `197b094d`, `6b00e833`, mesh indireto, 4 opções do CP | Portados em `a35427a6`, `fe728422`, `56c08517`, `cecb3eea` |

Existem no código mas estão fora do preset U59: `KYTY_BDA_SHARED_BLOCKS`, `KYTY_STREAM_RING_HOST`, `KYTY_VMA_BUDGET_CACHE_MS`, `KYTY_MOVREL_SWITCH`, `KYTY_UNIFORM_LANE_READS` e `KYTY_SHORT_F32_HELPERS`.

## 8. Ordem sugerida

1. ~~Par SGPR das máscaras (`08a3215e`, `Translate.cpp`)~~ **portado**, com o teste (`--scalar-mask-pair-only`, CTest `scalar_mask_pair`).
   - O nosso wave size vem do `CS_W32_EN` do dispatch (`pm4.h:919`), então a correção só age quando o guest pediu wave64. Em wave32 nada muda.
   - Sem a correção, o caso wave64 escreve em `s19:s20` e destrói o sentinela `s20`. Com ela, passa.
   - Continuam passando: `wave_reductions`, `wave_halves`, `wave_reductions_fold_lane_masks`, `wave_reductions_linear_uses`, `isa_accuracy_linear_uses`, `fold_lane_masks_codegen` e `wave_row_reduction_codegen`.
   - No `--new-opcodes-only` passam todos os casos até a falha antiga `VectorF64ModesModifiersAndExec`, e passam também os que vêm depois dela (F16, `ScalarBrevB64OperandsAndMasks` 32/64).
   - Fica pendente o `ReadU32Pair` das outras fontes SGPR de 64 bits.
2. O log do `vmaCreateImage` e a deduplicação do efeito de gatilho, se houver interesse.
3. BVH64 e `2e8eae14` (o do Bry), só quando um log ou uma medida de jogo nosso pedir.
