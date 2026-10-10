# Radeon: buffers somente de leitura por binding

## Escopo e decisão

Implementação experimental, controlada por `KYTY_READONLY_BUFFER_BINDINGS=1` no
início do processo. O padrão é desligado. A opção global `KYTY_READONLY_BUFFERS`
também precisa estar ligada (seu padrão já é ligado).

O gargalo medido anteriormente no Crash 4 foi CPU/sincronização: 68,44% das
amostras de RenderThread estavam numa rotina convidada de fila. Isso não prova
que shaders sejam o limite da cena. Esta frente atende à prioridade de
read-only por binding do roteiro Radeon e prepara uma comparação isolada.
Ainda não deve ser considerada uma otimização de FPS aprovada.

## Causa e mudança

Antes, um único array reunia todos os buffers do shader. Um store impedia que
esse array recebesse `NonWritable`, mesmo quando outros buffers eram usados
somente para leitura. Agora, o plano pode separar os recursos de leitura em
outro binding e decorar a variável desse array com `NonWritable`.

- `ResourceMaterialization.cpp::ProveReadOnlyBuffers` verifica os intervalos
  reais dos descritores a cada materialização. Inclui margem de alinhamento de
  256 bytes e rejeita sobreposição com qualquer buffer escrito/atômico.
- Buffers coerentes, escritas por endereço/BDA e imagens escritas/atômicas
  mantêm o caminho conservador. Descritores desconhecidos ou nulos também não
  autorizam uma prova de ausência de alias.
- A prova booleana entra na especialização e no cache. Endereços não entram
  na chave; mudar para descritores sobrepostos invalida a especialização RO.
- `BindingLayout.cpp`, o emissor SPIR-V e `pipeline/descriptors.cpp` usam os
  dois grupos. Os offsets de shader data conservam seus índices originais.
- O fallback de um recurso RO adiado/não mapeado usa zeros em armazenamento de
  upload, evitando alias com o buffer nulo compartilhado que aceita escrita.
- Fingerprint e codecs incluem a nova opção/prova; o hash das fontes invalida
  caches persistentes incompatíveis.

No teste sintético com buffers separados, o RADV emitiu `s_buffer_load_b32`
no lugar de `buffer_load_b32`; o caminho com alias preservou a leitura vetorial.
Isso comprova scalarização desse acesso específico, sem demonstrar menos VGPR
ou melhor FPS no jogo.
Os riscos são prova de alias incorreta, indexação dos grupos e reutilização
incorreta do cache. A separação pode aumentar declarações SPIR-V e custo de
binding; por isso precisa de A/B antes de virar padrão.

## Métricas e aceitação

| Métrica | Antes | Depois |
| --- | --- | --- |
| Binding RO no shader misto do teste, buffers separados | ausente | presente quando habilitado |
| Binding RO no teste com alias | ausente | ausente |
| SPIR-V do teste separado: palavras / instruções | 416 / 97 | 438 / 102 |
| ISA AMD do teste separado: bytes / instruções | 140 / 28 | 132 / 27 |
| Load do teste separado | `buffer_load_b32` | `s_buffer_load_b32` |
| SPIR-V do teste com alias: palavras / instruções | 416 / 97 | 416 / 97 |
| ISA AMD do teste com alias: bytes / instruções | 140 / 28 | 140 / 28 |
| FPS, 1% low, frame time médio/p95/p99 | sem baseline desta alteração | não medido |
| Thread_Gpu, CPU por thread, utilização/tempo de GPU | sem baseline desta alteração | não medido |
| DrawPrep/DrawRun, fallbacks, continued, waited, after CP stop | sem baseline desta alteração | não medido |
| Readbacks, waits, tempo/quantidade de submits, barriers | sem baseline desta alteração | não medido |
| Criações de pipeline e tempo de compilação | sem baseline desta alteração | não medido |
| SPIR-V/ISA de shaders do jogo, VGPR/SGPR, spills, scratch, occupancy | sem baseline desta alteração | não medido |
| VRAM, transferências RAM/VRAM, page/write faults | sem baseline desta alteração | não medido |

Imagem do jogo: comparação visual pendente. Readback sintético não substitui
essa comparação nem um teste prolongado de estabilidade. Manter a opção
desligada até comprovar um critério de ganho do roteiro sem regressão visual.

## Validação funcional

- `resource_materialization_tests`: passou, incluindo alias exato/parcial,
  margens de alinhamento, descritores nulos, coerência, atomics, BDA e flag off.
- `resource_tracking_tests`: suíte completa passou após corrigir o fixture FMASK.
- `shader_cfg_tests --readonly-buffers-only`: passou, com validação SPIR-V,
  codecs e mudança de especialização ao passar de buffers separados para alias.
- `--program-cache-only`: passou, incluindo fingerprint das 48 opções.
- `--readonly-buffer-bindings-only`: quatro readbacks GPU passaram com
  `KYTY_TEST_VULKAN_VALIDATION=full`, sem erros de validação Vulkan.
- `--buffer-publication-only` com a nova opção ligada: Wave32, Wave64, DLC e
  variantes atômicas passaram, sem erros de validação Vulkan.
- `--fmask-only`: mapeamento FMASK e textura inteira de comparação passaram
  na GPU, sem erros de validação Vulkan. Os dois fixtures receberam descritores
  válidos para o buffer de readback.
- Build de `kyty_emulator` e dos testes concluído. Executável e mapa atualizados
  em `_Build/linux-clang/install` e `_Build/crash-run`, com backups
  `*.before-readonly` no diretório de logs.

O loader registrou um aviso ao ignorar o ICD DZN; o teste usou RADV. O dump ISA
foi capturado com `RADV_DEBUG=shaders,nocache`, conforme o
[guia de depuração ACO](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/amd/compiler/README.md).
Essas opções são diagnósticas e não foram colocadas no perfil do jogo.
Artefatos: `readonly-spirv-stats.json`, `readonly-isa-stats.json`, os quatro
arquivos `readonly-*.isa` e `readonly-gpu.log`, sob o diretório de logs abaixo.

## FMASK e RenderThread

O caminho FMASK já substitui o load por mapeamento constante de amostras e
remove o descritor de imagem. Seu teste GPU exige ausência de `OpTypeImage` e
`OpImageFetch`. Não há evidência de custo residual que justifique outra
otimização desse caminho nesta cena.

Uma falha preexistente em `TestFmaskLoadSpecialization` foi reproduzida com
fontes de HEAD em build isolado: o buffer de saída tinha stride zero e bounds
mode zero, tornando seus stores fora dos limites. DCE removia o resultado que
o teste tentava inspecionar. Corrigir o bounds mode do fixture fez a suíte de
HEAD passar; não foi necessário alterar a semântica FMASK de produção.

O controle ao vivo da espera de DrawPrep e as medições de RenderThread estão
em [RENDERTHREAD-2026-10-09.md](RENDERTHREAD-2026-10-09.md). O jogo precisa estar
na mesma cena para comparar os orçamentos de espera; reduzir spin sem medir
pode trocar consumo de CPU por latência de acordar.

Logs locais desta validação: `_Build/renderthread-20261009/readonly-*.log`.
