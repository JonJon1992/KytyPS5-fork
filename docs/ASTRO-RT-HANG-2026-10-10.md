# Astro's Playroom: travamento GPU no CS `7d6ab87e6c984bb7` — análise — 2026-10-10

Análise somente leitura, feita enquanto outra sessão (Codex) editava o
recompiler e os testes. Nenhum código foi alterado, nenhum build foi feito e o
jogo não foi aberto para este documento. **O travamento não está resolvido.**
Há uma causa provável com evidência forte, mas sem prova direta da instrução
que trava. VGPR, spills e FPS não foram medidos.

Complementa `docs/ASTRO-RT-2026-10-09.md` e a seção "Hipótese delimitada:
lista sem marcador residente" de `docs/HARDWARE-RT-BACKEND-2026-10-10.md`.

## 1. Travessia do shader

Disassembly: `_Build/rt-integration-20261010/astro-sources/stage4_7d6ab87e6c984bb7.bin.rdna2`.
SPIR-V real (cache do backup, idêntico ao controle):
`_Build/rt-integration-20261010/resume-baseline/shader/7d6ab87e6c984bb7-0.spvasm`.

A travessia é cooperativa por wave: a wave inteira percorre o BVH junto, e
todo o estado de travessia fica em SGPRs.

| Laço | Faixa | Saída |
|---|---|---|
| TLAS | `0x350..0xdd8` | pilha vazia (`s16 == 0`, `0xdc4`) |
| BLAS | `0x4a0..0x9bc` | pilha vazia (`s16 == s50`, `0x9a8`); pilha em lanes de `v10` (`V_WRITELANE`/`V_READLANE`) |
| Lista de folhas | `0x50c..0x5a8` | **somente `s35 < 0`** (`0x58c`) |

Montagem dos recursos da BLAS (`0x3c4..0x49c`), para cada instância em
`s46 = s48 + (s36 >> 3) * 160`:

- `s[60:61]`: ponteiro da BLAS (instância +8). O descritor BVH `s[24:27]` usa
  `base = ptr >> 8` e `size = (u64 em BLAS+80) - 1`, com `s27 |= 0x81000000`.
- O V# da lista `s[60:63]` tem base igual à base da BLAS e stride 8
  (`S_BITSET1_B32 s61, 19`). `num_records` vem de `s62 = s42`, isto é, do
  dword em BLAS+88. `s63` é `0x00016204`.
- A raiz da BLAS é `s51 = 37` (`0x25`, box32 no índice 4) ou o dword em
  instância+152.
- A instância é ignorada quando o dword em instância+0 é ≤ 255.

O laço de lista é este:

```
0x4d8  S_BUFFER_LOAD_DWORD   s34, s[60:63], s51 & ~5        ; nó da primeira entrada
0x4ec  (s34 & 7) == 7 → sai
0x4fc  S_BUFFER_LOAD_DWORD   s35, s[60:63], (s51 & ~6) + 4  ; id da primeira entrada
0x520  IMAGE_BVH_INTERSECT_RAY ... s34 ...                   ; testa o nó do par
0x58c  S_CMP_LT_I32 s35, 0 ; S_CBRANCH_SCC1 → sai
0x5a0  S_BUFFER_LOAD_DWORDX2 s[34:35], s[60:63], s52 * 8     ; próximo par, s52++
0x5a8  S_BRANCH 0x50c
```

No SPIR-V é o laço `%74` (merge `%81`). Ele sai com `OpSLessThan %2661 0`.
`%2661` vem de `load_bda_u32` (`%262`), com o limite
`stride == 0 ? records : stride * records` (`LoadIndirectScalarBuffer`,
`spirvEmitterMemory.cpp`). A tradução é fiel ao guest.

## 2. Leituras que devolvem zero

- Página BDA ausente: `get_bda_pointer` (`DefineGetBdaPointer`,
  `spirvEmitterMemory.cpp`) devolve 0 e chama `RecordBdaFault`. A leitura
  devolve 0.
- Fora do limite do V#: `in_bounds == false` e a leitura devolve 0, como no
  hardware.
- `FaultManager::ProcessFaultBuffer` só cria os buffers das páginas pedidas
  (`FindBuffer` com 16 KiB, `CACHING_PAGEBITS = 14`) depois que o trabalho que
  produziu o readback termina. A residência avança em ondas, de dispatch em
  dispatch.

Comportamento de cada caminho quando os dados estão ausentes:

| Caminho | Dado ausente | Resultado |
|---|---|---|
| Nó TLAS/BLAS (`bvh_intersect`) | página do nó ausente | `box_invalid` = 4×`0xFFFFFFFF`; nenhum filho empilhado; termina |
| Instância (`S_LOAD` +0) | zero | ≤ 255 → instância ignorada; termina |
| Lista de folhas | zero em `s35` | **não é terminador**; `s52` só cresce; depois do limite o OOB devolve 0 para sempre |

Portanto, o único caminho que não termina com dados ausentes é este: um nó
residente cujo filho folha aponta para uma lista em página ausente, sem
nenhum `s35` negativo residente até o limite. O dispatch não termina, o
readback nunca acontece e a página nunca é materializada. O resultado é um
timeout gfx.

## 3. Evidência

### 3.1 Execução que travou (`resume-baseline/`, binário `541e3a03…`)

`grep "Accessed non-GPU cached memory" kyty.txt` encontra **exatamente oito
páginas em toda a sessão**, e todas são da região do BVH:

```
411757  0x403b28000                      ← página da raiz (BLAS 0x403b28600)
411825  0x403b2c000 0x403b30000 0x403b34000 0x403b3c000
        0x403b48000 0x403b4c000 0x403b50000
```

Os dois lotes chegam por volta de 64 s de execução. São o último trabalho de
GPU processado: o log termina em cerca de 66 s com `ErrorDeviceLost`. A
primeira operação em voo no travamento é o dispatch
`7d6ab87e6c984bb7 [1,2048,1]`. As páginas `0x403b38000`, `0x403b40000` e
`0x403b44000`, que ficam no meio da BLAS (cerca de 0x818 nós × 64 B a partir
de `0x403b28600`), nunca foram reportadas. Isso é coerente com elas terem sido
lidas no dispatch que travou, cujo readback não ocorreu, mas não prova que
foram.

A região está dentro do mapeamento `GpuOnionMemory`
(`0x400000000`, `0xb200000` = 178 MiB, linha ~2988 do `kyty.txt`).
As páginas não foram tocadas pela GPU antes de 64 s. Isso indica um BVH
carregado ou escrito pela CPU, não construído por compute; não é prova.

### 3.2 Execução com `KYTY_LOOP_GUARD=256` (`capture-sampling-incident/loop-probe/`)

Essa execução não travou. Foram 53 páginas ausentes, todas em regiões de BVH
(`0x403af4000…`, `0x4043f8000…`, `0x404cf8000…`), materializadas em ondas.
Correlação com o contador do guard em 773 atualizações:

| Período | Atualizações | Esgotamentos médios por atualização |
|---|---:|---:|
| Enquanto ainda havia páginas ausentes | 20 | 340.419 |
| Depois das 53 páginas residentes | 753 | 35.413 |

A primeira atualização do guard aparece logo depois do primeiro lote de
faltas. O resíduo posterior é compatível com travessias cooperativas longas
legítimas, porque a wave visita a união dos nós dos 64 raios. O orçamento de
256 não distingue um laço longo de um laço infinito, e a imagem preta dessa
execução vem do corte de travessias legítimas.

Script da correlação (somente leitura):

```sh
grep -n "Loop guard:\|Accessed non-GPU cached memory" \
  _Build/rt-integration-20261010/capture-sampling-incident/loop-probe/kyty.txt
```

## 4. Hipóteses restantes

1. **Limite do V# no `S_BUFFER_LOAD`.** Falta conferir no manual RDNA2 se
   `num_records` está em unidades de stride ou em bytes. O emulador usa
   `stride * num_records`. Um limite maior que o do hardware não perde o
   marcador; só um limite menor perderia.
2. **Pilha em lanes ou reconvergência.** Os testes `--traversal-lanes-only`
   passaram. Essa hipótese não explica a correlação com as páginas ausentes.
3. **Dados obsoletos ou sincronização.** É pouco provável, pelo que diz 3.1.
   Seria possível se a CPU escrevesse o BVH depois da materialização e o
   rastreamento de escrita perdesse essa escrita.

## 5. Confirmação

O probe em andamento (`KYTY_SCALAR_READ_PROBE_*`, PCs `0x4fc`/`0x5a0`,
`ProbeScalarRead`) separa as hipóteses pelo campo `reason`:

| Resultado | Interpretação |
|---|---|
| `reason=3` (página não mapeada) e o jogo deixa de travar | mecanismo da seção 2 confirmado |
| `reason=1` (fora do limite) | limite do descritor errado (hipótese 1) |
| probe nunca dispara e o travamento continua | hipótese rejeitada; voltar a 4.2/4.3 |

Alternativa definitiva, que exige root: aumentar `amdgpu.lockup_timeout` e
usar `umr --waves` durante o travamento para obter o PC das waves presas.
Depois, mapear esse PC para o ISA do RADV (`RADV_DEBUG=shaders`).

O probe aborta as invocações com leitura inválida e encerra o emulador de
propósito (`EXIT`). Ele serve só para diagnóstico.

## 6. Correção proposta

**Laços terminados por falha de residência**, no recompiler, sem limite de
iterações.

1. Em `DefineGetBdaPointer`, no `fault_label`, que já chama
   `RecordBdaFault`, gravar `true` numa variável `Private bool bda_faulted`,
   inicializada com `false` no início de `main`. O custo é zero quando não
   há falha.
2. Nos desvios condicionais que saem de laço, reaproveitar o gancho existente
   do `KYTY_LOOP_GUARD` (`EmitStructuredTerminator`,
   `spirvEmitterProgram.cpp`, `IsLoopMergeBlock`), trocando `exhausted` por
   um voto uniforme: `any(bda_faulted)` via `ctx.Ballot`, que já cobre wave64
   sobre subgroup32 (`other_half`). A saída precisa ser uniforme, porque os
   laços do guest são uniformes por construção (`S_CBRANCH_*`) e há
   `V_READLANE`/shuffles depois deles.
3. Aplicar somente a shaders com `uses_dma` e a laços sem barreira de
   workgroup. Se uma wave saísse de um laço com barreira, as outras ficariam
   esperando nela. O shader `7d6ab87e6c984bb7` tem zero `S_BARRIER`.

Contrato preservado:

- O valor das leituras não muda: página ausente continua 0, OOB continua 0,
  e nada passa a devolver `0xFFFFFFFF`.
- OOB **não** marca a flag, porque no hardware devolve 0 legitimamente. Um
  laço que dependa disso também travaria no PS5.
- Sem falha, a execução é idêntica à de hoje, bit a bit. Uma invocação que
  leu dado inventado pela residência preguiçosa sai dos laços em vez de
  travar a GPU. O quadro fica errado só enquanto a página não chega, como já
  acontece hoje com as leituras zero.
- Custo: um voto de subgroup por avaliação de saída de laço, contra uma
  iteração que já chama `bvh_intersect`.

Ponto de atenção: endereços dentro da abertura de 40 bits mas sem mapeamento
guest falham para sempre. Uma invocação que os leia teria os laços cortados
em todo quadro. Caso isso apareça, o host pode registrar essas páginas como
"zero residente" depois do primeiro readback, para que parem de marcar a flag.
Não implementar isso sem um caso real.

### Melhoria opcional, que não substitui a correção

Materializar o intervalo inteiro do descritor na falha (BVH:
`[base, base + (size+1)*64)`; V#: `stride * num_records`), em vez de uma
página de 16 KiB. Isso exige registrar o intervalo no buffer de falhas. A
fronteira converge em um quadro, o que reduz os quadros errados. **Sozinha
não garante término**: BLASs vizinhas compartilham a página da raiz (por
exemplo, `0x403b28000` contém o fim da anterior e o início de
`0x403b28600`), então ainda pode haver residência parcial.

### O que não fazer

- Não aumentar nem ligar o orçamento global de `KYTY_LOOP_GUARD`.
- Não devolver `0xFFFFFFFF` em páginas ausentes ou em leituras OOB.
- Não materializar o `GpuOnionMemory` inteiro (178 MiB) nem escrever código
  específico para PPSA01325.

## 7. Testes

Partir do fixture `--traversal-lists-only`, removendo o teto de 32 iterações
nos casos em que a correção deve terminar sozinha:

| Caso | Esperado |
|---|---|
| Lista residente, limite completo | mesmas leituras e mesmo número de iterações com e sem a correção; flag falsa |
| Página da lista ausente | laço termina; flag verdadeira; valores lidos = 0; falta da página registrada |
| Página residente, marcador além do limite | OOB devolve 0; flag **falsa** (manter o teto da fixture, porque a semântica é a do hardware) |
| Descritor com stride 0 / stride 8 | limite em bytes / `8 * num_records` |
| Nó BVH ausente | `bvh_intersect` devolve 4×`0xFFFFFFFF`; flag verdadeira |
| Wave32 e wave64 (inclusive meia wave ativa) | saída uniforme; nenhum erro de validação |
| Shader com barreira dentro de laço | gancho não aplicado (teste de codegen) |

Executar um teste por vez, sob `systemd-run --user --scope -p MemoryMax=12G`,
com validação Vulkan `full`.

## 8. Validação

1. `spirv-val --target-env vulkan1.2` no módulo do shader.
2. RADV: VGPR, SGPR, spills e tamanho de código antes e depois
   (`RADV_DEBUG=shaderstats` ou `VK_KHR_pipeline_executable_properties`).
3. Jogo, executado pelo usuário: Astro PPSA01325, mesma cena, sem
   `--game-patch`, `KYTY_BVH_CAPTURE_SHADER=0`, `KYTY_LOOP_GUARD=0`,
   `KYTY_HW_RT_BACKEND=0`. Verificar ausência de timeout gfx no kernel por
   pelo menos 3 minutos, imagem normal confirmada pelo usuário e FPS por
   flips do CP antes e depois (baseline em `docs/ASTRO-RT-2026-10-09.md`:
   20,82 fps, em outra configuração).
4. Registrar no log quantas vezes a flag disparou (contador por dispatch,
   lido depois de a GPU concluir) para provar que ela só aparece na fronteira
   de residência.
