# Radeon: análise de uniformidade no IR

## Escopo

Segunda frente de `prompt_otimizacao_radeon_rdna_kytyps5.md`. A medição ao vivo do
Crash 4 foi adiada a pedido do usuário. Esta implementação permanece experimental
sob `KYTY_UNIFORM_LANE_READS=1`, cujo padrão continua desligado.

A opção já informava uniformidade ao driver depois de emitir a leitura entre
lanes. Agora, `EliminateReadLane` consulta uma prova transitória antes da emissão
e substitui `ReadLane`/`ReadFirstLane` pelo source quando ele é idêntico em todas
as lanes convidadas, inclusive fora de EXEC. EXEC vazio continua lendo lane zero.

`UniformityAnalysis` propaga `Uniform`, `Divergent` e `Unknown`. Constantes,
`GetUserData` e `GetShaderBase` são origens uniformes; uma allowlist explícita de
operações puras conserva essa propriedade. Selects verificam condição e ambos
os braços, com exceção de braços idênticos. `Divergent` significa que o valor
pode variar entre lanes, sem provar que necessariamente varia.

Memória, coerência/polling, atomics, relógio, valores indefinidos, registradores
pseudo, phis e operações de subgroup permanecem `Unknown`. Não basta um endereço
ser uniforme para tornar uma leitura de memória uniforme. Phis de constantes
distintas também podem depender de controle divergente.

A análise usa memoização sob demanda e limite de profundidade de 128; o fallback
para expressões maiores é conservador. A flag desligada não consulta nem aloca
o mapa. Somente nós inicialmente `Unknown` são substituídos no pass, evitando
invalidar provas `Uniform` já armazenadas. A prova não é serializada: o
fingerprint já inclui a opção e o hash das fontes invalida caches anteriores.

## Comparação sintética na RX 9070 XT / RADV

O mesmo shader convidado foi compilado e executado com a opção desligada/ligada.
Seu source é `userdata + 7`, escrito em VGPR com EXEC completo; os testes incluem
seletor dinâmico, primeira lane ativa e EXEC vazio.

| Métrica | Wave32 desligado → ligado | Wave64 desligado → ligado |
| --- | --- | --- |
| Leituras `OpGroupNonUniformShuffle` | 3 → 0 | 3 → 0 |
| SPIR-V: palavras | 728 → 491 | 780 → 491 |
| SPIR-V: instruções | 166 → 117 | 176 → 117 |
| ISA AMD: instruções / bytes | 28 / 144 → 28 / 144 | 28 / 144 → 28 / 144 |
| Mediana de criação do pipeline, 5 amostras por opção | 0,9 → 0,8 ms | 0,9 → 0,8 ms |

O RADV já eliminava as leituras na ISA do fixture: não foi demonstrado ganho de
custo de execução GPU ou pressão de registradores. Os tempos de criação foram
medidos alternando as opções, sem cache do driver e sem validation layers;
a resolução do relatório é 0,1 ms. A variação em Wave32 (0,5–1,1 ms desligado,
0,5–1,4 ms ligado) impede concluir ganho consistente de compilação.

No fixture com escrita parcial em VGPR, as três leituras foram preservadas.
O SPIR-V cresce 15 palavras nessa comparação por causa do broadcast que a opção
já adicionava ao fallback: 779 → 794 em Wave32 e 842 → 857 em Wave64. A ISA
continua com 50 / 248 e 61 / 292 instruções / bytes, respectivamente.

FPS, 1% low, frame time médio/p95/p99, Thread_Gpu, CPU por thread, utilização e
tempo de GPU, DrawPrep/DrawRun, fallbacks, continued, waited, after CP stop,
readbacks, waits, submits, barriers, criações de pipeline em jogo, VGPR/SGPR,
spills, scratch, occupancy, VRAM, transferências e faults: não medidos nesta
frente. Comparação visual e estabilidade prolongada em jogo ficam para a retomada.
Manter desligado até o A/B em jogo comprovar benefício sem regressão.

## Evidência funcional

- Teste CPU novo falhou antes da implementação, mantendo a leitura uniforme.
- `scalar_provenance_tests`: prova positiva e fallbacks para divergência, EXEC
  parcial/vazio, memória, undef, relógio, phis e profundidade limitada.
- `shader_recompiler_compute_tests --uniformity-only`: readbacks exatos on/off
  em Wave32 e Wave64; módulos também validados para host32 e host64.
- `VectorReadFirstLaneEmptyExecReadsLane0`: preserva os resultados `{102,100}`
  com a flag desligada e ligada.
- A opção anterior continua marcando as leituras de fontes divergentes com
  broadcast; seus readbacks Wave32/Wave64 permanecem corretos.
- Validação Vulkan completa: zero erros; um aviso do loader ao ignorar o ICD DZN.
  A execução usou RADV. Layouts alternativos foram validados estruturalmente;
  execução usa o subgroup que o dispositivo fornece.
- `shader_uniformity` registrado no CTest passou; `--program-cache-only` passou
  com fingerprint das 48 opções e 153 testes de truncamento.
- `shader_cfg_tests --isa-accuracy-only` e `--wave-reduction-only` passaram com
  a opção ligada; `--readonly-buffers-only` passou com a opção desligada.
- Build de `kyty_emulator`, `shader_cfg_tests`, `scalar_provenance_tests` e
  `shader_recompiler_compute_tests` concluído; smoke `kyty_emulator --help` passou.

A suíte `shader_cfg_tests` completa **ainda não passa**: falha na verificação
`aligned scalar DWORDs must each use one BDA lookup`. Foi produzida uma comparação
isolada substituindo apenas `ReadLaneElimination.cpp` pela versão de HEAD e
mantendo os demais arquivos/fixtures atuais; ela reproduziu a mesma falha. Isso
confirma que a falha independe desta implementação de uniformidade, sem afirmar
que a suíte inteira foi validada. Evidência em `uniformity-before/result.json`.

A investigação também corrigiu descritores inválidos de três fixtures: bounds
do output `s[48:51]` no helper CFG, output do feedback bindless e output de gather
com LOD explícito. Com stride zero e OOB_SELECT zero, a especialização descartava
seus stores e DCE removia os resultados que as asserções tentavam inspecionar.
Os seletores bindless e gather passaram com descritores de raw offset bounds.

Executável e mapa atualizados em `_Build/linux-clang/install` e `_Build/crash-run`.
SHA-256 do executável: `6cf70abfbb48d34fa4405fd464b31259ae4f5f450ca8efc04484da8497ece077`.
Backups `*.before-uniformity` e manifesto `uniformity-install.json` ficam no
diretório de artefatos. A flag e os presets de jogo não foram habilitados.

Artefatos em `_Build/renderthread-20261009`: `uniformity-gpu.log`,
`uniformity-isa.log`, `uniformity-spirv/`, `uniformity-spirv-stats.json`,
`uniformity-isa-stats.json`, `uniformity-*.isa` e
`uniformity-pipeline-times.{log,json}`. Dumps ISA e cache desligado foram usados
somente para diagnóstico, sem alterar o perfil do jogo.
