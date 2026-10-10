# Radeon: indirect mesh com contador na GPU

## Gargalo e causa

Terceira frente da ordem final de
`prompt_otimizacao_radeon_rdna_kytyps5.md`. O teste ao vivo do Crash 4 permanece
adiado a pedido do usuário.

O caminho mesh já convertia um registro indirect sem contador na GPU. Quando o
pacote tinha `count_addr`, `RenderExecutor::DrawIndirectNative` recusava a
conversão mesmo com `max_count == 1`. O command processor então lia o contador
e os argumentos na CPU. Fontes escritas pela GPU exigiam readback e espera antes
de reconstruir o draw no host.

## Mudança e arquivos

`KYTY_NATIVE_INDIRECT_MESH_COUNT=1|on`, **desligado por padrão**, permite a
conversão de um registro com contador. Depende dos caminhos native indirect e
mesh existentes estarem habilitados. Pacotes com vários registros continuam
no fallback: um draw pode escrever os argumentos de outro.

| Arquivo / função | Mudança |
| --- | --- |
| `renderer/renderDraw.cpp`, `DrawIndirectNative` | Aceita um registro com contador sob a flag; contador zero comprovadamente limpo usa o caminho CPU sem conversão. |
| `renderer/renderDraw.cpp`, `ExecutePreparedDraw` | Entrega buffer e offset do contador ao conversor. |
| `renderer/meshIndirect.cpp/.h`, `Converter::Record` | Variante de pipeline criada sob demanda, com terceiro storage binding para o contador. |
| `shaders/gpu_mesh_indirect.comp` | Lê `min(count, 1)` antes dos argumentos; zero produz comandos vazios sem carregar o registro. |
| `CMakeLists.txt` | Gera e incorpora a variante SPIR-V; registra testes on, off e verify. |
| `tests/ShaderRecompilerComputeTests.cpp`, `CheckMeshIndirectDraw` | Exercita o pacote CP, fontes CPU/GPU, pixels e herança de `NUM_INSTANCES`. |

Os caminhos de renderer e shaders acima ficam sob `src/graphics/host_gpu/`.

O descritor do contador usa offset alinhado a `minStorageBufferOffsetAlignment`
e um índice de dword para o deslocamento restante. Ele funciona também quando
o contador limpo vem de um stream buffer sem device address. As dependências
de memória existentes cobrem os produtores dos dois buffers, a leitura compute,
os comandos indirect, os parâmetros mesh e a verificação após conclusão.

O slot conserva seu tamanho de 256 bytes. A variante registra o contador
limitado em um dword antes reservado. O verificador compara os comandos e
parâmetros com a conversão CPU dos argumentos copiados pela própria GPU.
Contador zero deve ter cópia de argumentos zerada. Isso não exige readback das
fontes convidadas no momento do draw.

O caso de contador zero já limpo foi refinado após o primeiro A/B mostrar custo
desnecessário de conversão. `TryReadGpuCleanBacking` recusa dados GPU-owned,
escritas/publicações pendentes e labels pendentes, sem esperar a GPU. Ao obter
zero, o renderer retorna ao fallback CPU, que relê o contador e preserva
`NUM_INSTANCES` se ainda estiver zero. Nenhum comando foi registrado nessa
decisão.

## Benefício e comparação na Radeon

Equipamento: RX 9070 XT, RADV GFX1201, Mesa 26.2.3, Vulkan 1.4.354. A mudança
evita o round-trip de fontes geradas pela GPU e reduz o trabalho serial do CP.
O mecanismo é genérico; a evidência de execução foi obtida nessa Radeon.

Cinco pares A/B, alternando a ordem das opções, no mesmo executável. O fixture
desenha em um alvo RGBA32F de 32×32 com blending aditivo. `DrawPrep=off` nos dois
tratamentos e validation layers desligadas durante a medição. O intervalo
inclui somente `CommandProcessor::DrawIndirectMulti`; leitura de pixels e
resolução posterior de instâncias ficam fora dele.

São 60 amostras por tratamento após excluir o primeiro pacote de cada processo,
que inclui a criação inicial da variante de pipeline. As opções readonly e
uniformity permanecem nos seus padrões desligados.

| Métrica sintética | Flag desligada | Flag ligada |
| --- | ---: | ---: |
| Readbacks nos 60 pacotes medidos | 55 | 0 |
| CPU por pacote, mediana | 164,327 µs | 31,946 µs |
| CPU por pacote, média | 315,834 µs | 32,105 µs |
| CPU por pacote, mínimo–máximo | 9,649–791,844 µs | 1,022–59,523 µs |
| CPU com contador zero limpo, mediana de 5 amostras | 12,684 µs | 1,082 µs |

A mediana agregada caiu 80,6% nesse fixture. Os 55 readbacks correspondem aos
11 casos que exigiam fontes GPU por processo; o contador zero limpo já não
fazia readback no controle. Todos os casos medidos mantiveram pixels e estado
de instâncias corretos. Os logs conservam as amostras iniciais excluídas e os
resultados por caso, evitando inferir ganho de FPS desses tempos de pacote.

| Helper SPIR-V | Palavras | Instruções |
| --- | ---: | ---: |
| Conversor sem contador, antes e depois | 1680 | 392 |
| Nova variante com contador | 1866 | 434 |

O módulo sem contador é idêntico byte a byte, SHA-256
`fa6c065cbeda01a2702a31cdfe26cf22b0e00f3d16752ce3a3df7d67ca755b95`.
A variante acrescenta código para o contador; não há alegação de redução de
SPIR-V, registradores ou custo GPU.

FPS, 1% low, frame time médio/p95/p99, `Thread_Gpu` ms/frame, CPU por thread,
utilização/tempo de GPU, DrawPrep/DrawRun, continued/key matches/fallbacks em
jogo, waited/after CP stop, readbacks por frame, waits, tempo/quantidade de
submits e barriers por frame, criações/compilação de pipelines em jogo,
VGPR/SGPR, spills, scratch, occupancy, VRAM, transferências e faults não foram
medidos nesta frente. A/B visual e estabilidade prolongada no Crash ficam para
a retomada.

## Validação, impacto visual e risco

- Teste novo falhou antes da implementação por readback e ausência da conversão
  com contador. O teste do contador zero limpo também falhou antes do ajuste.
- Oito CTests `mesh_indirect*` passaram com validação Vulkan completa; zero
  erros e nenhum erro ignorado. Há um aviso do loader ao ignorar o ICD DZN.
- `mesh_indirect_count` e `mesh_indirect_count_verify` passaram também com
  validação de sincronização: zero erros. Além do DZN, há um aviso da
  configuração de shader validation desse modo de teste.
- Treze casos por execução: contadores 0, 1, 9 e `UINT32_MAX`; indexado e
  não indexado; argumentos/contador GPU-owned ou CPU-clean; zero instâncias;
  transições entre contadores; offset do contador com desalinhamento de
  storage descriptor. As comparações de pixels RGBA32F são exatas.
- Um draw direto posterior verifica a herança: contador zero conserva as sete
  instâncias anteriores; contador positivo herda as instâncias do registro.
- Casos anteriores do conversor, incluindo 3 e 70.000 instâncias, continuam
  passando. O fixture termina a slice normal de CP para concluir o DrawPrep
  antes de ler pixels; a validação funcional usa o DrawPrep padrão.
- Build de `kyty_emulator` e `shader_recompiler_compute_tests`, smoke
  `kyty_emulator --help` e `git diff --check` passaram.

Os guards existentes de restart, clear, targets e limites do host permanecem.
Os limites anteriores do helper também permanecem: quatro dispatch records,
status de overflow de instâncias, limites de grupos e faixa de índices. A
conversão não acrescenta fallback para os status já existentes. Por isso a
ampliação permanece experimental; os testes não garantem correção para todos
os registros de um jogo.

Há custo de um compute dispatch, dos comandos mesh vazios quando o contador
GPU é zero e da criação inicial de um pipeline adicional. O teste de pacote
não mede esse custo GPU. Não houve crash ou device lost nas execuções aprovadas;
ausência de races, leaks ou regressões em sessões longas ainda requer validação.

A suíte CFG completa tem a falha anterior de BDA lookup registrada em
`UNIFORMITY-2026-10-09.md`; não é declarada aprovada aqui. O seletor específico
`--mesh-indirect-only` passou.

**Decisão:** manter a implementação sob flag desligada até o A/B em jogo.
Critério demonstrado: redução de readbacks e CPU de pacote, com pixels exatos
no fixture. A próxima frente na ordem do documento é DrawPrep/DrawRun.

## Executável e evidência

Executável e mapa atualizados atomicamente em `_Build/linux-clang/install` e
`_Build/crash-run`, com hashes verificados e backups. SHA-256 do executável:
`2de1633e5b488a34376f26477f9257ad52498b7c1539449d74515df3909b0c61`.
O preset de jogo e a flag experimental não foram habilitados por esta etapa.

Esse executável corresponde à frente indirect validada. Alterações paralelas
posteriores em `textureCache.cpp/.h` e `tools/profiles/PPSA02433-crash4.json`
foram preservadas e não fazem parte do build descrito acima.

Artefatos em `_Build/indirect-20261009/`: `red-test.log`,
`clean-zero-red-test.log`, `clean-zero-build.log`, `final-tests-full.log`,
`final-tests-sync.log`, `bench_count.py`, `bench-count.json`,
`bench-count-*-*.log`, `helper-spirv.json`, `vulkan-summary.log`,
`emulator-help.log`, `indirect-count-install.json` e backups
`{install,crash-run}-kyty_emulator{,.map}.before-indirect-count`.
O primeiro A/B fica preservado em `bench-before-clean-zero/`.
