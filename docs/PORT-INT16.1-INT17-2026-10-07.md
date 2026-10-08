# O que aproveitar do int16.1 e do int17-pre (Jetsku) na guest-sync-release-mem

Data: 2026-10-07. Base original: `guest-sync-release-mem` em `e1607c98`.

**Atualização da integração:** o B1/B2 selecionado está em `ffa2bcd9` e o lote A do Claude
(`port-int17-lote-a`, `9dac8a94`) foi integrado por merge na `guest-sync-release-mem`. A mudança
local ainda não commitada em `spirvEmitterMemory.cpp` não faz parte desse merge. A seção 2 conserva
a triagem original para explicar por que cada item foi escolhido.

**Regra do usuário:** as nossas otimizações ficam. Nenhum commit do upstream pode substituir ou reverter:

- a sincronização BDA (batch protect, incremental, coalescing de upload);
- o CP sequencer e o recorder;
- os certificados do DrawPrep;
- a velocidade de compilação de shader (IrAllocator, decoder por tabela, análise de CFG e EXEC);
- os loads de storage buffer sem branch;
- os strides em runtime e os bindless por shader;
- a chave do program cache.

**Divisão do trabalho:** o Claude fez o lote A (A1 + A2) e as correções encontradas nos testes; o
Codex portou os itens B1/B2 selecionados, preservando os caminhos otimizados existentes.

---

## 1. Situação atual

### 1.1 Feito

O lote A veio do branch **`port-int17-lote-a`** (topo `9dac8a94`, 32 commits sobre `e1607c98`),
no worktree `/home/jonathanbraga/kyty-int17a`. O B1/B2 foi commitado separadamente em `ffa2bcd9`.
As branches divergiram em `e1607c98`; a integração preserva ambos os históricos por merge.

| Item | Estado | Commits no branch |
| --- | --- | --- |
| Lote A1 + A2 (25 cherry-picks `-x`) | Feito, compila, testes passam | `db6b53c9` … `31b23aa1` (tabela 2.1) |
| Otimizações nossas por cima do lote A | Feito | `8efd153e`, `e44f56aa`, `9c3ce1af`, `c95de41b` (seção 1.2) |
| Astro's Playroom com cenário e Astro pretos | **Corrigido**: o usuário confirmou o jogo iluminado a 60 fps | `7638f642`, `9678735d` (seção 1.3) |
| Contador "Instances" do painel (~2³²/frame) | Corrigido | `9dac8a94` |
| `63789372` (estava no B2) | Portado independentemente nos dois lados; o merge consolidou uma implementação e um teste | `9678735d`, `ffa2bcd9` |
| Itens B1/B2 selecionados | Portados com oclusão U59 e DrawPrep preservados; caminhos indiretos/quiet ops experimentais continuam opt-in | `ffa2bcd9` |

O merge teve dois conflitos de conteúdo: `ShaderCFG.cpp` (implementações equivalentes da região
de break) e `SrtWalker.cpp` (leitura de tabela nula). A resolução conserva a estrutura CFG do
`ffa2bcd9` e a leitura nula do `7638f642`. O build ainda detectou duas cópias do mesmo teste
`TestTraversalLoopBreakRegion` após o merge automático; ficou uma cópia, acessível pelos dois
modos de teste. Nenhum arquivo da otimização local de BDA foi incluído no merge.

### 1.2 Otimizações nossas por cima do lote A

| Commit | O que faz |
| --- | --- |
| `8efd153e` | Estende o `KYTY_READONLY_BUFFERS` (`ceca7f7a`): page table BDA, shader data e flattened SRT ficam `NonWritable` em **todo** programa, inclusive nos que escrevem nos próprios buffers. É mais ganho de AMD, porque as cargas uniformes podem ir pelo scalar cache. Teste: `shader_cfg_tests --readonly-buffers-only`. O teste do upstream também falhava na nossa branch: o V# do teste era zero, e o `e129e07e` descartava o store. Agora o teste usa o buffer do fixture e exige que o store sobreviva. |
| `e44f56aa` | Corrige o bug 3 do upstream (seção 4): a fiber cuja entry retornou devolve a pilha. Isso acontece no `FiberRun`, depois que a thread sai da pilha, e só se o objeto ainda tem aquela pilha. Uma fiber reinicializada com o mesmo tamanho mantém a pilha que já tinha. Teste: `virtual_memory_allocation_tests --fiber-only` (`ReturnedFiberGivesStackBack`, que falha sem a correção). |
| `9c3ce1af` | Imagem que foi para a RAM do sistema (fallback do `5ccf9230`) não volta ao pool de imagens. A próxima imagem do mesmo formato tenta a VRAM de novo. |
| `c95de41b` | O prefetch do binding memo (`d40a7e53`) não roda num estágio com um único buffer, onde só repetiria o hash. |

### 1.3 Astro's Playroom preto: duas correções

O problema **já existia antes do port**. O binário base `e1607c98` também renderizava preto. Foi achado com o branch de diagnóstico `diag-materialize-trail` (`a1949860`), que **não é para integrar**: ele imprime os pontos de falha da materialização de um estágio descartado.

1. **`7638f642` SRT: uma tabela nula lida diretamente vira descritor zero.**
   - **Causa:** desde o `2211275e` (correção do Yotei), o `SrtWalker::ReadRawWord` sem reader fazia falhar toda leitura abaixo de 64 KiB. Os passes iluminados do Playroom leem tabelas não preenchidas em `0x28`, então todos esses draws eram descartados (milhares de "stage materialization failed without a retryable read ... dropped").
   - **Correção:** a 1ª página (abaixo de 0x1000) é lida como descritor zero, igual ao caminho com reader (`c6a98f60`/`f8c87b86`). De 0x1000 a 64 KiB continua falhando, que é o caso do Yotei.
   - **Teste:** `shader_cfg_tests --srt-null-pointer-only`, que falha sem a correção.
2. **`9678735d` CFG: merge de seleção para o condicional da região de break do loop** (porte do `63789372`).
   - **Causa:** com os passes de volta, o CS `0x5df28d2058caf728` gerava SPIR-V inválido ("Selection must be structured: OpBranchConditional"). O RADV travava em `radv_shader_spirv_to_nir` (`radv_shader.c:544`), escrevendo pelo NULL que o `spirv_to_nir` devolveu.
   - **Porte:** o `StructureBreakRegions` roda no fim do nosso `StructurizeImpl(graph, clone_tails)`, que mantém a clonagem de tails compartilhados e as otimizações de análise de CFG.
   - **Teste:** só o `TestTraversalLoopBreakRegion` foi junto (`shader_cfg_tests --break-region-only`; falha sem a correção). Os testes de RT stub/software do upstream dependem do RT do int16 e ficaram de fora.

### 1.4 Testes rodados no branch A

Cada um rodou isolado, com `systemd-run MemoryMax=12G`.

**Passaram:**

- `clear_register_decode`, `device_compat`;
- `hang_watchdog`, `hang_watchdog_fire`, `hang_watchdog_fatal_shutdown`;
- `page_manager`, `page_manager_protect_reuse`;
- `virtual_memory_allocation --fiber-only`;
- modos de `shader_cfg_tests`:
  - os novos: `--readonly-buffers-only`, `--srt-null-pointer-only`, `--break-region-only`;
  - os existentes: `--mesh-indirect-only`, `--wolverine-instructions-only`, `--runtime-buffer-stride-only`, `--flat-address-space-only`, `--typed-views-only`, `--indirect-buffer-loop-only`, `--packed-image-decode-only`, `--frontend-optimization-only`, `--isa-accuracy-only`, `--registered-shader-code-only`, `--post-dominators-only`, `--exec-select-analysis-only`, `--loop-shared-continuation-only`.

**Falha anterior ao port:** `shader_cfg_tests --bindless-only` ("bindless feedback did not use atomic OR"). Ela também falha no build principal `e1607c98-dirty`.

### 1.5 Pendente

- **Crash Bandicoot 4:** testar renderização e medir contra o binário base (comandos na seção 5).
- **Medição A/B limpa do A2** no Playroom, no mesmo ponto, com os caches aquecidos e sem build rodando (seção 5). As primeiras medições foram contaminadas: cache frio e build do Codex em paralelo.
- **B2 experimental:** medir os modos opt-in de DrawPrep/DrawRun e oclusão na cena-alvo antes de
  mudar seus padrões. Os testes anteriores no Yōtei não mostraram ganho repetível. O branch de
  diagnóstico `diag-materialize-trail` continua fora da integração.

---

## 2. Triagem

Os dois releases estão no repositório local como `refs/jetsku/int16.1` (`f9e19582`, igual ao `origin/main`) e `refs/jetsku/int17-pre` (`85907e05`). O int17 contém tudo do int16.1 e mais cerca de 20 mudanças. A nossa branch não tinha 72 commits do int17 (comparação por patch-id).

**Como foi verificado** (sem mexer na árvore de trabalho):

- `git merge-tree --write-tree --merge-base=C^` simulou o cherry-pick de cada commit em sequência, encadeando as árvores com `git commit-tree`;
- cada commit foi cruzado com os 314 arquivos que a nossa branch alterou desde `58a7f743`.

### 2.1 A. Sem conflito e sem afetar as nossas otimizações — FEITO

Os 25 entraram limpos, na ordem abaixo. Nenhum toca `spirvEmitterMemory.cpp`, `drawPrep`, `graphicsRun`, `faultManager`, `cpOps`, `ShaderCFG`, `Translate.cpp` ou `ShaderDecoder`. O único arquivo quente tocado é o `bufferCache.cpp` (`d40a7e53`).

#### A1. Correções e robustez (caminho frio)

| Upstream | No branch | O que faz | Observação |
| --- | --- | --- | --- |
| `cb84831b`, `6acf3a6a` | `db6b53c9`, `d06367bf` | Revertem "font: fix glyph metrics" e "font: add scaled kerning" | Esses commits quebravam o título de fase do Astro Bot. |
| `fb4819f0` | `5dc6becb` | Clear por registrador de alvos de 64 bits usa CLEAR_WORD1 (também R16F, RG16F, R16, RG16, R8, RG8, B10G11R11) | Desliga com `KYTY_CLEAR_REGISTER_WIDE=0`. |
| `5ccf9230`, `a1359303`, `20944479`, `4aa6cbc0` | `47d1138c`, `12a591d5`, `6edb6768`, `0cac8d20` | Sem VRAM, a imagem vai para a RAM do sistema e o buffer tenta de novo; `KYTY_VRAM_LIMIT_MB` simula uma GPU menor | Só age quando a VRAM acaba. Refinado em `9c3ce1af`. |
| `d68adba1` | `1b1c917d` | A busca de faixa livre começa na faixa que contém o endereço inicial | Mesmo resultado, mais rápido. |
| `33acf48a` | `cd18c9cb` | APR: um id de arquivo por caminho resolvido | `KYTY_APR_HASHED_IDS=1` volta ao antigo. |
| `d0111331`, `9ca918a8` | `785ab1d0`, `4fd4b2ad` | Views remapeadas recuperam a proteção do page tracker | No Linux, só o caminho do `mprotect` com escrita. `KYTY_REMAP_TRACKER_PROTECTION=0` desliga. |
| `42496743`, `34139a62` | `c68fc25c`, `b9127f5a` | Fibers pequenas (menos de 256 KiB) rodam em pilha própria | Bug 3 corrigido em `e44f56aa`. `KYTY_FIBER_GUEST_STACK=0` desliga. |
| `2e2acd94`, `69e6dfee`, `b0121469` | `9b459c8d`, `b324abb9`, `08a3c281` | Proteção do stack-walk do watchdog, proteção SEH e TerminateProcess | Só têm efeito no Windows. |
| `5c0497b9`, `90e9ff35` | `f2da6531`, `d7ef5081` | Log de capacidades da GPU e proteção de geometria só para Turing | Muda o fingerprint, então os shaders são recompilados uma vez. |
| `143aa870` | `a22ac03b` | "AMD CPU patch" passa a se chamar "Intel CPU compatibility" | Só texto. |
| `996c6873` | `f1d91443` | `--time-spv` nos testes | Ferramenta de teste. |
| `1967db9d`, `bc629b65` | `4ef0c607`, `07325370` | `KYTY_WIDE_FILL_CLEAR`, `KYTY_GUEST_STORAGE_REPEAT` | Desligados por padrão. |

#### A2. Mudam comportamento ou desempenho (medir antes e depois)

| Upstream | No branch | O que faz | Observação |
| --- | --- | --- | --- |
| `ceca7f7a` | `31b23aa1` (+ `8efd153e`) | `NonWritable` nos storage buffers de programas que não os escrevem (`KYTY_READONLY_BUFFERS`, ligado) | Ganho para AMD/RADV. Segurança conferida: todo store e atomic passa por `BufferAccessOf`. |
| `d40a7e53` | `2e825366` (+ `c95de41b`) | Prefetch dos slots do binding memo (`KYTY_CP_BINDING_BATCH_PREFETCH`, ligado, live) | Só uma dica de cache. Medir junto com o binding memo guard (`7b2537c1`). |
| `26ec6420` | `56c08517` (+ `9dac8a94`) | Draws de mesh indireto montados na GPU por padrão (`KYTY_NATIVE_INDIRECT_MESH`) | Tira a espera de readback da CPU. `empty` volta ao antigo. |

### 2.2 B. Com conflito, ou que pode afetar as nossas otimizações — port selecionado em `ffa2bcd9`

As contagens de blocos de conflito abaixo foram medidas **depois** do lote A e registram a triagem
original. `ffa2bcd9` portou os itens selecionados com testes focados. A exceção opcional de
`avPlayer.cpp` (`3baa8490`) ficou fora. O modo de oclusão U59 permanece no padrão, e os caminhos
de DrawRun indireto e quiet ops permanecem opt-in após as medições no Yōtei.

#### B1. Conflito pequeno em código que não é de otimização (portado à mão, mantendo o nosso lado)

| Commit | Conflito | Valor | Como portar |
| --- | --- | --- | --- |
| `19e8d65f` Oclusão: pares de ZPASS por endereço | 1 bloco, `occlusion.cpp` | **Alto**: mantemos `KYTY_GPU_OCCLUSION=1`. Corrige a lava do vulcão. | Manter as reduções pelo command sink (`7eba18be`) e trocar só o pareamento. |
| `1f011415` Start instance vai para a SGPR | 1 bloco, `vulkanWindow.cpp` | Demon's Souls: pilares e paredes de pedra. | **Corrigir junto o bug 1.** |
| `f321ea10` Pula a validação do IR em release | 1 bloco, `Translate.cpp` | Tradução mais rápida. | Só a metade do `SpirvEmitter.cpp:461`, com o nosso gate `ValidateTranslatedIr()`. |
| `5d053044` AudioPropagation + log de import não resolvido | 1 bloco, `runtimeLinker.cpp` | Também corrige uma corrida no stub thunk. | Manual. |
| `8ed0c214` Crash report com a cadeia de chamadas do host | 1 bloco, `runtimeLinker.cpp` | Diagnóstico. | Junto com o `5d053044`. |
| `3baa8490` Diagnóstico de áudio e crash (Sndz) | 1 bloco, `avPlayer.cpp` | Diagnóstico. | Opcional. |
| `e420d246` Níveis de áudio e sliders de volume | 5 blocos | Médio. | Manual. |
| `d09339da`, `e37d7b92` Objetos 3D do AudioOut2 e downmix 7.1→estéreo | 19 blocos cada | Médio (Astro Bot). | `KYTY_AUDIO_STEREO_DOWNMIX` desligado por padrão: deixa o som cerca de 13 dB mais alto e pode clipar. |
| `224ca3ab` Opção "Accurate occlusion" no launcher | 11 blocos | Baixo. | Só a opção. **Não** trocar `KYTY_GPU_OCCLUSION` para 0 no preset. |

#### B2. Conflito em código que nós otimizamos (portado com controles para medição A/B)

| Commit | Conflito | O que tocaria | Valor e risco |
| --- | --- | --- | --- |
| `81841608` Draw runs na torre do relógio (`KYTY_OCCLUSION_SPLIT`, `KYTY_DRAW_RUN_QUIET_OPS`, `KYTY_DEPTH_FEEDBACK_LAZY`) | 3 blocos, `drawPrep.cpp`/`.h` | Certificados do DrawPrep e `KYTY_DRAW_PREP_RELEASE_OVERWRITTEN` | **Alto no modo de oclusão precisa:** neve de 17.5 para 25 fps no upstream. |
| `1d75e7cb` Draws indiretos preparados continuam draw runs | 5 blocos | CP sequencer e DrawPrep | Médio. Depende do `81841608`. |
| ~~`63789372`~~ CFG: merge do condicional da região de break | — | — | **Feito em ambos os branches e consolidado no merge.** |
| `9336bd00` Leitura FLAT de endereço não mapeado devolve 0 | 3 blocos | `SrtWalker` e `memory` | Robustez (Yotei, Wolverine). |
| `7f36c7d7` Pula o programa em vez de sair | 5 blocos | `pipelineCache` e `SrtWalker` | Robustez. |
| `35646655` Dump dos callees pulados por S_SWAPPC_B64 | 4 blocos | `pipelineCache`, `ShaderRecompiler` | Diagnóstico. |
| `02ad2709`, `0bf6a3c1` Estatísticas | 1 bloco cada | Caminho quente | Baixo. |

#### B3. Não portar: substituem ou competem com o nosso trabalho

| Grupo | Commits | Motivo |
| --- | --- | --- |
| Ray tracing em software do int16 | `6c785330` `676f003b` `626a7f23` `16ba1add` `348fd9d3` `9ea0ffb5` `e34ec0f9` `463523fd` `91eae246` `94eded90` | Concorre com o nosso BVH do Wolverine (`spirvEmitterBvh.cpp`) e com o decoder por tabela. Se valer a pena, fazer como projeto separado. O `463523fd` tem o bug 2. |
| `KYTY_BDA_WRITES` | `dbe6e32d` `33131e70` (e parte do `94eded90`) | Mexe na nossa sincronização BDA. Só faz sentido junto com o RT do int16. |
| `555db311` SRT_VARIANT_READS em todo raw load | — | Já temos o nosso caminho de stride em runtime e bindless. |
| Merge de regiões de readback | `31870cd9` `f8c2b169` `4d0208b9` | Desligado no upstream; mexe no `bufferCache`. |
| `KYTY_DRAW_RUN_QUIET_SYNTHETIC` | `d7228c94` `4f4b8d36` | Só ajuda com a oclusão desligada. |
| Inversão do preset | parte do `224ca3ab` e do `463523fd` | Manter `KYTY_GPU_OCCLUSION=1` e `KYTY_DRAW_PREP_RELEASE_OVERWRITTEN=1`. |

### 2.3 C. Já cobertos na nossa branch (não aplicar)

| Commit | Onde já está |
| --- | --- |
| `4a6519a1` Endereço além da tabela de páginas BDA = não mapeado | O nosso `get_bda_pointer` já rejeita endereços fora da abertura de 40 bits (`in_aperture`, `spirvEmitterMemory.cpp`, perto da linha 1322). |
| `e481d122` Ponteiro SRT nulo como descritor zero | Caminho com reader: `f8c87b86`, depois de uma leitura que falhou. Caminho direto: `7638f642`. Aplicar o `e481d122` desfaria o refinamento do `f8c87b86`. |
| `f321ea10`, metade do `Translate.cpp` | `ValidateTranslatedIr()` / `KYTY_VALIDATE_IR`. |

### 2.4 D. Só notas de release e CI

`c8670cef` `576d42d0` `3520347d` `ff58eb46` `289df3bc` `5c25169e` `44e5d2df` `833e6fc6` `85907e05`. Também o workflow `u59-tests.yml`, que está no int16.1 e não está no int17.

---

## 3. Releases analisados

- **int16.1:** tag `u59-windows-20261007-int16.1` = `f9e19582`.
- **int17-pre:** tag `u59-windows-20261007-int17-pre` = `85907e05`.
- **Os zips:** os mesmos 93 arquivos. Mudam só `kyty_emulator.exe`, `launcher.exe` e o preset (o int17 acrescenta `KYTY_RT_SOFTWARE=auto`).

---

## 4. Bugs do upstream

Conferidos no código do int17.

1. **ID de instância de mesh dividido em vários dispatches** (`1f011415`, B1, **corrigido em `ffa2bcd9`**):
   - **Falha anterior:** `Translate.cpp` e os draws divididos misturavam a base de cada dispatch
     com `first_instance`; instâncias após o primeiro segmento recomeçavam do zero.
   - **Correção atual:** `renderDraw.cpp` e `meshIndirect.cpp` enviam o deslocamento do dispatch e
     `first_instance` em dwords separados; `Translate.cpp` calcula v8 a partir dos builtins.
     `KYTY_START_INSTANCE_SGPR=0` não substitui essa correção.
2. **Crash ao fechar com pipelines de RT na fila** (`463523fd`, B3): `DropPending()` destrói o único dono de cada `std::promise`, e o `future.get()` lança `broken_promise`. Só importa se o RT for portado.
3. **Vazamento da pilha de fiber** (`42496743`): **corrigido em `e44f56aa`**. A pilha é devolvida no `FiberRun` quando a fiber termina, em vez de mudar o `FiberFinalize`.
4. **Linux sem proteção SEH** (`69e6dfee`): `CallCatchingStructuredException` só chama a função. Só importa para o RT em segundo plano (B3).

**Riscos que ficaram sem confirmação:**

- um `mprotect` somente leitura não ressincroniza páginas que o tracker mantém NoAccess (`d0111331`);
- `SetAttributes` do AudioOut2 pega `g_audioout2_port_mutex` para atributos que não são PCM (`d09339da`).

---

## 5. Como testar no jogo

Cada diretório tem binário (ou symlink), preset e `_PipelineCache` próprios. `assets`, `_SaveData` e `_Patches` são symlinks do install principal. O install principal, que o Codex usa, não foi tocado.

Depois do merge, `_Build/linux-clang/kyty_emulator` é o binário compilado da branch integrada.
Para o smoke test do Yōtei, `_Build/cpu-gpu-sync/profile.py` executa esse binário com o preset
U59 e `tools/patches/PPSA05512-performance-no-rt.json`. O `install/kyty_emulator` só passa a
representar o merge depois de uma instalação explícita; não confundir seus logs com os do build.

- Para jogar, use o `run-u59.sh` do worktree do binário com `KYTY_RUN_DIR=<diretório>`. Ele aplica o preset U59.
- A primeira abertura de cada diretório só aquece o cache; meça a partir da segunda.
- Confira a linha `Source build` no log.

| Diretório | Binário | Para quê |
| --- | --- | --- |
| `kyty-int17a/_Build/run-int17a` | `9dac8a9` (lote A + correções), **com o launcher do mesmo build** (`run-int17a/launcher`) | Uso normal: abra o launcher dali, ou rode `run-u59.sh --game <eboot>`. O install principal (launcher `365aa45`, emulador `9a9827f-dirty`) não foi tocado. |
| `kyty-int17a/_Build/run-int17a-ro0` | o mesmo, com `KYTY_READONLY_BUFFERS=0` | A/B do `NonWritable`. |
| `kyty-int17a/_Build/run-int17a-off` | o mesmo, com todas as chaves do lote A desligadas | Isolar regressão do lote A. |
| `kyty-int17a/_Build/run-int17a-val` | o mesmo, com `--shader-validation true`, `KYTY_SHADER_VALIDATION_ASYNC=0` e log em `_kyty.txt` | Pegar SPIR-V inválido antes do driver; o log cresce rápido. |
| `kyty-int17a/_Build/run-int17a-crash` | `9dac8a9`, linha do launcher do Crash 4 com o patch `_Patches/PPSA02433.json` | Crash 4. Rodar **sem** `--game`. |
| `kyty-base-e1607c9/_Build/run-base` | `e1607c9` (antes do port) | Comparação. |
| `kyty-base-e1607c9/_Build/run-base-crash` | `e1607c9`, linha do launcher do Crash 4 com o patch | Comparação no Crash 4. Rodar **sem** `--game`. |

Exemplos:

```sh
# Astro's Playroom com o lote A
KYTY_RUN_DIR=/home/jonathanbraga/kyty-int17a/_Build/run-int17a /home/jonathanbraga/kyty-int17a/tools/run-u59.sh --game /run/media/jonathanbraga/44E4157B8EEA41E3/PS5/PPSA01325-AstrosPlayroom/eboot.bin 2>&1 | tee ~/astro.log

# Crash 4 com o lote A e com o binário base
KYTY_RUN_DIR=/home/jonathanbraga/kyty-int17a/_Build/run-int17a-crash /home/jonathanbraga/kyty-int17a/tools/run-u59.sh 2>&1 | tee ~/crash-int17a.log
KYTY_RUN_DIR=/home/jonathanbraga/kyty-base-e1607c9/_Build/run-base-crash /home/jonathanbraga/kyty-base-e1607c9/tools/run-u59.sh 2>&1 | tee ~/crash-base.log
```

Chaves para A/B no mesmo binário (acrescentar no fim do comando):

- `KYTY_READONLY_BUFFERS=0` (muda o codegen: use um diretório próprio, senão o cache é invalidado);
- `KYTY_CP_BINDING_BATCH_PREFETCH=0`;
- `KYTY_NATIVE_INDIRECT_MESH=empty`.

## 6. Primeiras medições (Astro's Playroom, RX 9070 XT / RADV)

Contaminadas por cache frio e por um build do Codex em paralelo. **Não usar como conclusão.**

| | Padrão (`c95de41`) | `KYTY_READONLY_BUFFERS=0` |
| --- | --- | --- |
| FPS | 35.4 | 25.6 |
| 1% low | 28.6 | 14.6 |
| `Thread_Gpu` | 97% | 96% |
| CPU waiting on GPU | 437 ms/s | 478 ms/s |

Esses números são de antes das correções do Playroom, quando os passes iluminados ainda eram descartados. Depois das correções, o hub mostra 60 fps (limite do vsync), 1% low 58.4 e `Thread_Gpu` 65%.

## 7. Validação da integração na `guest-sync-release-mem`

O merge entre `ffa2bcd9` e `9dac8a94` foi compilado no build Linux/Clang: `kyty_emulator`,
`shader_cfg_tests`, `shader_recompiler_compute_tests`, `clear_register_decode_tests`,
`device_compat_tests`, `hang_watchdog_tests`, `page_manager_tests` e
`virtual_memory_allocation_tests`. `git diff --check` não encontrou erros de whitespace nem
marcadores de conflito.

- **Host/CTest:** 9/9 (`clear_register_decode`, `device_compat`, três modos de `hang_watchdog`,
  dois de `page_manager`, pós-dominadores CFG e região de break).
- **Shader CFG:** 7/7 modos focados: `--readonly-buffers-only`, `--srt-null-pointer-only`,
  `--break-region-only`, `--mesh-indirect-only`, `--runtime-buffer-stride-only`,
  `--flat-address-space-only` e `--typed-views-only`.
- **Fiber:** `virtual_memory_allocation_tests --fiber-only` passou, inclusive o retorno da
  pilha após o fim da entry.
- **Vulkan/CTest:** 7/7 com acesso à GPU (`mesh_indirect` padrão, verify, on, off, codegen,
  `srt_variant_reads` e `srt_unmapped_reads`). No sandbox, o primeiro teste Vulkan não progrediu;
  a repetição com acesso ao dispositivo terminou em 1,28 s.

O Yōtei iniciou duas vezes com o patch Performance (`PPSA05512-performance-no-rt.json`), e ambas
as execuções terminaram normalmente, sem erro fatal ou `VK_ERROR_DEVICE_LOST` no log. A primeira
amostra estava fria: em 45–65 s, 6,65 fps e 861 ms/s de espera de compilação. Na segunda, entre
70–90 s, houve 20,15 fps (49,63 ms/frame), 1.187 waits GPU/s, 756 ms/s esperando GPU, 38 mil
trocas de contexto/s e 3.177 write-tracking faults/s; ainda havia 93 ms/s de espera de compilação.
Os dados estão em `_Build/cpu-gpu-sync/claude-merge-yotei-20261007` e
`_Build/cpu-gpu-sync/claude-merge-yotei-warm-20261007`. **Não são A/B limpo** contra o build
anterior: a cena e o cache de shaders não foram fixados entre versões. O gargalo de waits continua
aberto, sem evidência de ganho ou perda de FPS atribuível apenas a este merge.

O binário desses testes também contém a edição local ainda não commitada de
`spirvEmitterMemory.cpp`; ela ficou fora do commit de integração. Uma validação do commit exato,
sem essa edição, precisa de um build limpo separado.

## 8. Compilação e limpeza após a integração

No Linux sem IPO, `kyty_full_emulator_objects` compila as 221 fontes comuns uma vez e as liga
ao emulador e aos testes que usam o mesmo conjunto de fontes. `virtual_memory_allocation_tests`
continua com objetos próprios porque usa `KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS=1` e
`-fexceptions`; builds com IPO e outras plataformas mantêm a compilação anterior. No grafo
Ninja, os nove alvos completos passaram de **1.998 para 451 compilações C++** (menos 77,4%).
Para `kyty_emulator`, `shader_cfg_tests` e `shader_recompiler_compute_tests` juntos, passaram
de 666 para 224. Cada executável ainda tem seu link próprio; os números são de compilações
planejadas, não uma medição de tempo de build ou FPS.

Após essa mudança, os 10 testes CTest de host selecionados passaram, assim como oito testes
com acesso à GPU (`kernel_file_system`, cinco modos de `mesh_indirect`, `srt_variant_reads` e
`srt_unmapped_reads`). Também passaram os sete modos focados de `shader_cfg_tests` e
`virtual_memory_allocation_tests --fiber-only`. O teste `kernel_file_system` precisa de acesso
ao dispositivo Vulkan; falhou no sandbox sem GPU e passou fora dele.

A limpeza do diretório de trabalho removeu 1.734 saídas antigas que já não constavam do grafo
Ninja, a configuração CMake incompleta em `build/` (sem executável) e 52 arquivos temporários
`.o.tmp` de compilações interrompidas. `_Build/linux-clang`, perfis A/B, caches e binários
de comparação foram mantidos. A edição local de BDA em `spirvEmitterMemory.cpp` não foi alterada.
