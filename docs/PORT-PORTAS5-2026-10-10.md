# Triagem do fork BannedPenta01/PortaS5 — 2026-10-10

Fork voltado a handhelds AMD (ROG Ally / Ryzen Z1, Radeon 780M, Batocera, RADV). Base local:
`guest-sync-release-mem` @ `33ed62e3`. Refs buscadas em `refs/portas5/*`.

| Branch | Topo | Relação com o nosso histórico |
|---|---|---|
| `master` (tag `v0.3.0-fork.1`) | `bd4fcda1`, 2026-10-08 | **Histórico desconectado**: raiz `71ce91ac` ("baseline + phase1-3 wip"), import raso do upstream `main` de 2026-09-27 (`421684e`). 14 commits do autor "dev". |
| `astro-handheld-pack` | `31a01734`, 2026-10-09 | `master` + 2 commits (vídeo multithread, catálogo de patches). |
| `bannedps5` | `ec970dd8`, 2026-10-10 | Deriva do upstream (merge-base `58a7f743`, int15). 60 commits: 52 são da linha upstream int16/int16.1 (já triados em `PORT-INT16.1-INT17-2026-10-07.md` com outros hashes) e 8 são do BannedPenta01. |

Só leitura: nada foi compilado nem rodado. Os números do fork são do Z1/780M com Smurfs Vileaf (PPSA02876) e Astro's Playroom.

## Resumo

| Item | Commit(s) | Veredito |
|---|---|---|
| Decode de vídeo multithread + conversão NV12 direto no buffer do guest | `462fb89b` (= `b256536f`) | **Portar (avPlayer)**; videodec2 só com teste |
| Resolução de display reportada ao guest | `026c9bdb` | **Portar como knob opt-in** |
| Achados de RADV no Astro (iluminação preta, hang de watchdog) | `b10df297`, `943a2d21`, `ec970dd8` | Informativo |
| Pipeline assíncrono que pula draws/dispatches | `bd4fcda1` | Pular |
| Cache de programas com LRU limitado por RAM | `2e7dd191` | Pular |
| Upscaler FSR1 no present | `c015f49c`, `90464448`, `23041fec` | Pular por ora |
| Perfil de desempenho handheld (detecção de APU/RAM) | `2e7dd191`, `23041fec` | N/A (temos `tools/profiles`) |
| Catálogo de patches da comunidade no launcher | `4282c612`, `31a01734` | Pular |
| Instalador de PUP, `--firmware-info`, contadores de FPS, relatórios, CI | vários | N/A |

## Portar

### 1. Vídeo multithread (`462fb89b`) — risco baixo, `git apply --check` passa limpo

- **avPlayer:** `thread_count` escala com os núcleos (máx. 6, `KYTY_VIDEO_THREADS` sobrescreve) e `thread_type = FRAME|SLICE`.
  - O ffmpeg abre com `thread_count=1` por padrão; hoje não setamos nada (`git grep thread_count src/libs` vazio).
  - Cutscenes HEVC 4K são o caso que mais ganha.
- **`PrepareVideo`:** o `sws_scale` escreve direto nos planos do buffer do guest, que já tem o pitch certo, e o NV12 nativo é copiado linha a linha.
  - Isso elimina um `av_frame_get_buffer` de ~12 MB por quadro e uma passada inteira de cópia.
  - O `memset` do buffer inteiro antes da conversão continua (era assim antes; bastaria limpar o padding).
- **videodec2:** cuidado com `FF_THREAD_FRAME`. A threading por quadro atrasa a saída em `thread_count-1` quadros.
  - O avPlayer tem filas assíncronas, então lá não importa.
  - No `sceVideodec2Decode`, o jogo pode esperar uma imagem por AU. Portar o videodec2 só com `FF_THREAD_SLICE`, ou testar num jogo que use videodec2 antes de ligar o FRAME.
- **Na 9070 XT:** o ganho só aparece em cutscene de vídeo. A Thread_Gpu não muda.

### 2. Resolução reportada (`026c9bdb`) — knob opt-in, A/B pelo usuário

- **Hoje:** `VideoOutGetOutputStatus` (`videoOut.cpp:1848-1850`) reporta 4K (`2u`). Só reporta HD (`1u`) se o `ATTRIBUTE3 & 4` do param e a janela forem menores que 4K. É idêntico ao do fork.
- **No fork:** `--display-resolution Auto|HD|FullHD|UltraHD`. No Smurfs (UE4), o backbuffer caiu de 3840x2160 para 1920x1080, com ~2x fps no Z1 (cena limitada por pixel).
- **Para nós:** o Crash 4 e o Yotei são limitados pela Thread_Gpu, então o ganho deve ser pequeno. Mas é um knob de ~20 linhas que vale um A/B, sobretudo em telas abaixo de 4K e em jogos limitados por GPU.
  - Fazer como env (`KYTY_DISPLAY_RESOLUTION=hd|uhd`), sem o "Auto = handheld".
  - HD e FullHD mapeiam ambos para `1u` no fork; o enum do SDK não distingue.
- **Risco:** alguns jogos podem fixar a resolução dos render targets por outros caminhos. O knob só muda o que o jogo escolhe.

## Informativo — achados de RADV no Astro's Playroom (780M)

- **Iluminação preta:** sem `KYTY_SRT_VARIANT_READS=1`, os kernels de GI eram pulados. Já está no nosso `tools/u59-preset.json:51`.
- **Orçamento de nós do RT em software:** `KYTY_RT_NODE_BUDGET` (upstream `348fd9d3`). O padrão 8192 deixava a viewport preta e 262144 iluminava.
  - Não temos esse RT: o pacote de RT em software do int16 ficou como projeto separado em `PORT-INT16.1-INT17`, e o nosso caminho é o BVH próprio / hardware RT.
- **Hang em cenas pesadas, não resolvido:** os dispatches de iluminação do BVH em software estouram o watchdog de ring do amdgpu (~10 s), e o resultado é `vkQueueSubmit ErrorDeviceLost`.
  - Reproduziu com 65536 e com 262144.
  - VRAM foi descartada.
  - As linhas `SRT: reads unmapped address 0x28` são benignas.
- **Relevância para a `astro-rt-hang-analysis`:** é o mesmo sintoma de loop/travessia longa.
  - O `amdgpu.lockup_timeout` não muda em runtime, mas pode ir como parâmetro de boot do kernel (`amdgpu.lockup_timeout=...`). Isso serve para diagnosticar (separar "lento" de "infinito") no Fedora do usuário, não como correção.
  - A correção real seria dividir ou encurtar os dispatches.

## Pular

- **`bd4fcda1`, pipeline assíncrono:** um único worker com mutex global. Quando o pipeline não está pronto, **descarta o draw ou o dispatch** naquele quadro.
  - Descartar dispatch de compute corrompe resultado persistente (culling, BVH, buffers).
  - A tradução continua síncrona; só o `vkCreate*Pipelines` sai da thread.
  - Já temos `KYTY_ASYNC_PIPELINES`, `KYTY_ASYNC_TRANSLATE`, o precompile e o `stagePrepWorker` (`pipelineCache.cpp:370`).
- **`2e7dd191`, LRU de programas:** limite de 256/512/1024 por nível de RAM, varredura O(N) a cada evicção e destruição dos módulos.
  - O Yotei tem milhares de programas; isso causaria thrash de retradução.
  - O próprio fork reverteu o cache de módulos de rect-list desse commit por use-after-destroy (`70435bb2`).
- **FSR1 (EASU+RCAS no present, `c015f49c` + `90464448` + `23041fec`):** pular por ora; detalhes na seção abaixo.

## FSR1 em detalhe

**O que é:**
- Fontes MIT vendorizadas em `3rdparty/fsr1/` (`ffx_a.h`, `ffx_fsr1.h`) e dois compute shaders, `fsr1_easu.comp` e `fsr1_rcas.comp`.
- A classe `FsrUpscaler` (~330 linhas) cria os dois pipelines, um sampler e uma imagem intermediária do tamanho da janela.
- A ativação é por `--upscaler Fsr1`.

**Como engata** (`swapchain.cpp`, `RecordPresentCommands`):
- Só entra quando o quadro do guest é **estritamente menor** que o swapchain numa dimensão e não maior na outra.
- Nesse caso, a cadeia passa a ser EASU (fonte → intermediária) e RCAS (intermediária → imagem do swapchain como storage, em layout `General`).
- No lugar dele ficaria o blit bilinear. O box downscale (`KYTY_PRESENT_BOX_DOWNSCALE`) tem precedência.
- O swapchain ganha `eStorage` no `imageUsage` quando a opção está ligada.

**Por que não engata para nós hoje:**
- Reportamos 4K ao jogo (ver item 2 de "Portar"), então o flip buffer do guest é ≥ janela e o FSR nunca entra.
- Jogos de PS5 com resolução dinâmica fazem o upscale interno (TAAU/FSR2 do próprio jogo) e entregam o flip buffer na resolução de saída. O FSR1 do present não vê a resolução interna.
- Ele só faz sentido **junto com o knob de resolução reportada**: o jogo renderiza a 1080p e o FSR1 sobe para uma janela de 1440p/4K. Com isso, a UI do jogo também sai em 1080p reescalada.

**Ganho esperado na 9070 XT:**
- O Crash 4 e o Yotei são limitados pela Thread_Gpu (CPU), e cortar pixels quase não muda isso.
- O par resolução + FSR1 serve a jogos limitados pela GPU, ou a quem quer 4K na tela sem pagar 4K de render.

**Problemas para resolver se portar:**
- **Uso storage no swapchain sem checagem:** não se verifica se `surface.capabilities.supportedUsageFlags` inclui `eStorage`, só o `eStorageImage` do formato.
  - No RADV/X11 costuma existir, mas em alguns compositores Wayland ou drivers o `createSwapchain` falha.
  - É preciso checar e cair no blit.
- **Formato:** escrever direto na imagem do swapchain exige formato UNORM com storage. O nosso swapchain escolhe `B8G8R8A8Unorm`/`R8G8B8A8Unorm` (`swapchain.cpp:388-392`), o que é compatível; um swapchain sRGB ou HDR não seria.
- **Espaço de cor:** o EASU espera entrada perceptual (gamma). O flip buffer do guest em UNORM está em gamma, o que serve. Com saída HDR/PQ precisaria revisão.
- **Barreiras:** a do `to_transfer` vem de `eTransfer`, igual ao caminho atual; não há problema novo.
- **Integração:** o patch não aplica limpo no nosso `swapchain.cpp` (conflito em ~297, perto do `m_filter`). A adaptação é pequena.
- **Custo:** dois dispatches por present, na ordem de décimos de ms a 4K na 9070 XT (estimado, não medido).

**Veredito:** portar só se o knob de resolução reportada mostrar ganho num jogo limitado pela GPU. Os dois juntos formam a opção "render menor + FSR1"; sozinho, o FSR1 não faz nada nos nossos jogos.
- **Catálogo de patches:** baixa índice e arquivos de `raw.githubusercontent.com/BannedPenta01/ps5-game-patches` em execução. É conteúdo de terceiro sem verificação, e já temos fluxo de patches no U59.
- **`dladdr` no log de crash (`70435bb2`):** temos o call chain do upstream (`8ed0c214`); opcional.
