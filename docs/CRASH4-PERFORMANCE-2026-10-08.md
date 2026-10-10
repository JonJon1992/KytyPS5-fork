# Crash 4: rastreamento de escrita e uploads no Linux

## Ajuste aplicado

O preset U59 habilita duas opções nativas existentes:

```json
"KYTY_BDA_BATCH_PROTECT": "1",
"KYTY_UFFD_WP": "1"
```

Também foram aplicadas à instalação local em
`_Build/linux-clang/install/u59-preset.json`. As demais entradas locais foram
preservadas; a cópia anterior está em
`_Build/crash-perf-20261007/preset-install-before-20261008.json`.

BDA coleta os uploads de cada passe, aplica toda a proteção e só depois copia
os bytes para o staging. O caminho mantém a publicação e as barreiras Vulkan.
No Linux compatível, userfaultfd substitui a proteção de escrita por `mprotect`;
a proteção sem acesso de páginas pertencentes à GPU continua usando `mprotect`.
Falhas e kernels incompatíveis acionam o caminho de fallback existente.

A janela adaptativa permanece no padrão. Neste host, userfaultfd foi calibrado
no nível 1, correspondente a 512 KiB. Com `mprotect`, o piso Linux é nível 2,
correspondente a 1 MiB. A comparação inicial entre backends fixou ambos em
1 MiB; a comparação seguinte testou 512 KiB separadamente.

Os pontos de implementação são `bufferCache.cpp::RunBdaPass`,
`GuestAddressSpace::ProtectTrackedPieceUnlocked` em `memoryAddressSpace.inc`
e `Common::UffdWriteWatch`. A implementação desses caminhos é igual entre
o binário medido e o commit `192afd16` da instalação.

## Ambiente e método

- Crash Bandicoot 4, PPSA02433, versão 01.000.002.
- Fedora 44, kernel `7.2.8-200.fc44.x86_64`, Radeon RX 9070 XT/RADV.
- Binário congelado: `a603778`, build ID
  `0a8dd14987aa0b516f5b966d54f4673d98e9dfd6`.
- Janela e patch do jogo em 1920×1080, Mailbox, áudio e oclusão habilitados.
- Mesmos comandos e caches na instalação isolada
  `_Build/crash-perf-20261007`; startup e navegação excluídos das janelas abaixo.
- O usuário confirmou manter personagem e câmera parados na área lenta.
- HangTrace foi habilitado nas comparações. Os números incluem seu custo.
  `perf` foi usado separadamente para localizar o trabalho da CPU.
- FPS = flips / tempo observado, com intervalos completos do CSV.
  Tempos de CPU somam threads; esses totais não são a latência de um quadro.

O `7,1 FPS` da captura original é o **1% low**. A taxa solicitada de flips
na captura era aproximadamente 13/s. O uso total de CPU e a falta de uma
medição de utilização da GPU não demonstram, sozinhos, um gargalo de shader.

## Medições

### Proteção BDA agrupada, alternada na mesma execução

| Estado | Intervalo aceito (ms) | Amostras | FPS | Proteções/s |
|---|---:|---:|---:|---:|
| Desligada | 80434–105595 | 25 | 18,76 | 41363 |
| Ligada | 111633–128742 | 17 | 19,93 | 23472 |

A amostra ligada teve cerca de 6,2% mais flips/s e 43,3% menos chamadas de
proteção/s. O trabalho de desenho permaneceu próximo de 1200 draws/quadro;
houve aproximadamente 2% mais draws/quadro na janela ligada. O pequeno
intervalo de retorno desligado serve apenas de apoio à comparação.

### userfaultfd, mantendo BDA ligada e janela de 1 MiB

| Backend | Intervalo aceito (ms) | Amostras | FPS | CPU em falhas (ms/s) | CPU em proteção (ms/s) |
|---|---:|---:|---:|---:|---:|
| mprotect | 95463–114594 | 19 | 17,35 | 463,8 | 582,5 |
| userfaultfd | 95567–114688 | 19 | 21,97 | 253,6 | 316,3 |

Essas execuções chegaram à mesma área, com pequenas diferenças de trabalho:
aproximadamente 1281 draws/quadro com mprotect e 1223 com userfaultfd.
O ganho isolado de FPS do backend permanece incerto nessa comparação entre
execuções. O custo `findbuf` dos últimos intervalos caiu de aproximadamente
30 para 21 µs por draw que inicia um run, coerente com o menor custo de proteção.
Compilação de shaders/pipelines foi praticamente nula nas janelas aceitas.

### Janela de escrita, com BDA e userfaultfd ligados

Alternância automática na mesma execução: 1 MiB → 512 KiB → 1 MiB.

| Janela | Intervalo aceito (ms) | Amostras | FPS | Upload de buffer (MiB/quadro) | CPU em proteção (ms/s) |
|---|---:|---:|---:|---:|---:|
| 1 MiB inicial | 80435–100565 | 20 | 21,66 | 66,13 | 304,8 |
| 512 KiB | 111642–139836 | 28 | 22,42 | 56,00 | 255,5 |
| 1 MiB retorno | 151911–176998 | 25 | 19,25 | 79,16 | 318,6 |

Draws/quadro ficaram próximos de 1250 depois do aquecimento. Em 512 KiB,
os uploads/quadro caíram aproximadamente 15,3% frente à primeira janela,
enquanto as falhas de escrita/quadro aumentaram de 177 para 231. Os custos
totais de falha e proteção caíram. A diferença entre os dois controles de
1 MiB mostra deriva do estado de rastreamento; a porcentagem exata de ganho
de FPS precisa de mais repetições para ser generalizada.

Uma tentativa anterior de 256 KiB (`trace-window`) mudou de cena e foi
descartada como evidência de desempenho.

## Verificação

Nove testes passaram com `KYTY_UFFD_WP=1` e `KYTY_BDA_BATCH_PROTECT=1`:

- `memory_tracker`, `memory_tracker_protect_reuse`;
- `page_manager`, `page_manager_protect_reuse`;
- `virtual_memory_allocation`;
- `bda_new_buffer_batch_protect`, `bda_new_buffer_batch_protect_sync_verify`;
- `bda_new_buffer_batch_protect_live0`, `bda_new_buffer_batch_protect_live1`.

Os testes BDA leem os bytes de volta da GPU e exercitam proteção e mudanças
de opção. O log detalhado confirmou userfaultfd ativo nos quatro testes BDA,
incluindo memória privada. Não houve falhas registradas de UffdWriteWatch
nas sessões do jogo. Os oito testes de rastreamento/BDA também passaram
anteriormente com mprotect.

Log final: `_Build/crash-perf-20261007/verify-final-gpu.log`.
Leitura das medições: `_Build/crash-perf-20261007/summarize_window.py`.
CSVs, logs e relatórios JSON permanecem nesse diretório local ignorado pelo Git.
Os presets de uso normal mantêm HangTrace desligado e não incluem os
contadores de diagnóstico adicionados às execuções isoladas.

## Pendências observadas

O perfil ainda tem trabalho significativo em `RenderThread 1`, cópias de
memória e sincronização. Os FPS medidos são amostras dessa área e desse host;
60 FPS e desempenho em outros jogos permanecem sem validação.

O usuário confirmou ausência de áudio também antes do ajuste. Com
`KYTY_AUDIO_LEVELS=1`, no gameplay os objetos entregam PCM com RMS aproximado
de −18,5 dBFS, mas o bed final e a saída SDL ficam em −180 dBFS. A mixagem
AudioOut2 precisa de investigação própria; a ausência de áudio permanece.
