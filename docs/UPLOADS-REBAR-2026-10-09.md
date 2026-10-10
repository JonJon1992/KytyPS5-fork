# Radeon: uploads, BAR e RAM cacheada

Frente solicitada após barriers: seção 10 de
`prompt_otimizacao_radeon_rdna_kytyps5.md`. O Crash 4 continua fora desta medição.

## Resultado e decisão

O caminho para pequenos dados já existe: o ring `MemoryUsage::Stream` usa
mapeamento persistente, escrita sequencial e preferência por memória do device.
`KYTY_STREAM_RING_HOST=1`, opção já existente, seleciona RAM host-visible/cacheada.
Não foi criada outra política equivalente nem alterado o padrão.

O RADV desta máquina expõe 15,75 GiB de VRAM não mapeável pela CPU e um heap
device-local/host-visible separado de **256 MiB**. Não expõe toda a VRAM à CPU.
Isso não permite concluir qual opção de BIOS está selecionada. O ring é de
64 MiB; os buffers grandes de staging continuam com preferência por RAM.

A alocação efetiva foi confirmada, não inferida apenas da preferência VMA:

| Caminho | Tipo Vulkan | Flags | Ring |
| --- | --- | --- | --- |
| Padrão, BAR | 3 | `0x7`: device-local, host-visible, coherent | 64 MiB |
| `KYTY_STREAM_RING_HOST=1` | 5 | `0xe`: host-visible, coherent, cached | 64 MiB |

Os dois modos produziram dados corretos. A comparação sintética não demonstra
um vencedor geral nem ganho de FPS; manter o padrão até A/B representativo.
Não foram alterados BIOS, preset, instalação do jogo ou política de alocação.

## Medição

Novo seletor `--stream-upload-paths-only` usa o ring real do BufferCache.
Cada lote contém 1.024 cópias CPU, alinhadas a 256 bytes, seguidas de leitura
pela GPU via copyBuffer e comparação completa em um buffer de download.
Cada cópia contém seu índice, para detectar offsets ou reutilização incorretos.
Há seis rodadas por tamanho; a primeira é descartada das estatísticas.

Cinco pares de processos alternados BAR/RAM, sem validation layers: 25 amostras
por tamanho e modo. Alocação e verificação ficam fora dos intervalos medidos.
O tempo CPU inclui Map/Copy/Commit e preparação das regiões; não mede sozinho
a conclusão das escritas pelo PCIe. O tempo de conclusão inclui gravação,
submissão, execução e espera CPU; **não é timestamp GPU**.

| Bytes/cópia | CPU BAR, ns/cópia | CPU RAM, ns/cópia | Conclusão do lote BAR/RAM, µs |
| ---: | ---: | ---: | ---: |
| 64 | 45,7 | 34,7 | 1118,7 / 1201,3 |
| 256 | 53,8 | 47,8 | 1205,6 / 1115,6 |
| 4096 | 628,3 | 745,1 | 1276,0 / 1358,8 |
| 16384 | 1401,7 | 1511,9 | 2058,5 / 2343,3 |

São medianas. As faixas CPU se sobrepõem: em 64 B, BAR 30,4–63,8 ns versus
RAM 29,6–51,1 ns; em 16 KiB, BAR 1089,7–3844,5 ns versus RAM 1126,0–3241,5 ns.
Não há isolamento de núcleo nem controle de frequência. O harness mantém seus
contextos de teste, incluindo dois rings de 64 MiB; ambos tiveram a localização
confirmada. Isso não representa a pressão de memória do jogo.

O consumidor é o copy engine, não shaders de constantes/vertex data. A medição
não resolve o custo das leituras por shaders sobre PCIe, frames em voo,
contenção CPU real, FPS, 1% low ou p95/p99. Não usar estas medianas para prometer
ganho no Crash ou habilitar RAM globalmente.

## Entrega e validação

- `KYTY_VRAM_STATS=1` agora registra tipo de memória, flags reais, tamanho e
  presença de mapeamento das alocações Upload/Stream. Diagnóstico desligado
  por padrão; nenhuma decisão do allocator foi modificada.
- CTests novos `stream_upload_paths_host0` e `stream_upload_paths_host1`:
  readbacks completos nos dois modos.
- Passaram os cinco CTests focados: os dois novos, `stream_buffer_ring`,
  `stream_buffer_ring_small_upload` e `stream_buffer_ring_small_upload_queued`.
- O teste de wrap/lifetime do ring também passou separadamente com
  `KYTY_STREAM_RING_HOST=1`.
- Validação de sincronização Vulkan: zero erros. Dois avisos por execução:
  configuração de sync validation e ICD DZN ignorado pelo loader; GPU RADV.
- Dez processos de medição terminaram com todos os readbacks corretos.
- Build de emulador/teste, smoke `--help` e `git diff --check` passaram.

Referência consultada via Context7: [VMA, padrões de uso](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/blob/master/docs/html/usage_patterns.html)
e [mapeamento persistente](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/blob/master/docs/html/memory_mapping.html).
Escrita sequencial e acesso aleatório/cacheado têm contratos distintos; a
alocação efetiva e sua coerência devem ser verificadas, especialmente sob pressão
do heap BAR. Flush, invalidação, fences e lifetime foram preservados.

Artefatos: `_Build/uploads-20261009/`, incluindo `vulkaninfo.txt`, `validation.log`,
`host-ring-validation.log`, `bench-*.log`, `bench-*.vram.log`, `summary.json`,
`runs.json` e logs de build/smoke. Os resultados não resolvem as falhas gerais
de outras suítes já documentadas nas frentes anteriores.
