# Radeon: avaliação de async compute

Décima frente da ordem final de `prompt_otimizacao_radeon_rdna_kytyps5.md`.
Resultado: capacidade presente, mas implementação adiada por ausência de
evidência de trabalho independente com ganho no jogo. Nenhuma mudança no
renderer, configuração de filas, preset ou instalação nesta frente.

## Hardware e caminhos atuais

Inventário RADV/RX 9070 XT coletado nesta sessão em
`_Build/uploads-20261009/vulkaninfo.txt`:

| Família | Filas | Capacidades relevantes |
| --- | ---: | --- |
| 0 | 1 | graphics, compute, transfer, sparse |
| 1 | 4 | compute, transfer, sparse |
| 2 | 1 | video decode |
| 3 | 1 | video encode |
| 4 | 1 | sparse |

Existe família compute separada. Não existe uma segunda fila na família
gráfica nem família exclusiva de transferência entre as expostas pelo driver.
Quantidade de filas não demonstra execução simultânea nem ganho de desempenho.

`VulkanCreateDevice`, em `src/graphics/presentation/window/vulkanWindow.cpp`,
solicita a fila universal e, quando disponível, uma segunda fila da mesma
família para side-copy. Nesta GPU, a segunda não existe: o caminho compartilha
a fila 0. A seleção de UploadDma exige transfer sem graphics/compute; a família
1 é rejeitada por também suportar compute. Isso é uma restrição da política
atual, não incapacidade dessa família de executar cópias.

As filas de compute do guest passam pelo `GuestGpu` e seu `RenderContext`
compartilhado (`graphicsRun.h`). `DispatchDirect` e `DispatchIndirect`, em
`renderCompute.cpp`, emitem no fluxo atual do renderer. Paralelismo no CP,
recorder e submissão CPU não significa que dispatches executem em uma segunda
fila Vulkan.

## Por que não mover dispatches nesta etapa

A ordenação atual depende do scheduler/timeline compartilhado e do estado
mantido pelos caches. `Image::GetBarriers` rastreia layout/stage/access por
imagem ou sub-recurso, não um histórico independente por fila. Os buffers
usuais são exclusivos; o compartilhamento entre famílias é habilitado apenas
nos caminhos que o solicitam explicitamente, como UploadDma.

Distribuir trabalho pela família 1 exigiria identificar RAW/WAR/WAW por recurso
e range, sincronizar produtores/consumidores com semáforos, tratar ownership
ou sharing e preservar lifetime/aposentadoria enquanto ambas as filas usam
o recurso. Labels, WAIT_REG_MEM/RELEASE_MEM, BDA, aliases e readbacks também
precisam manter sua ordenação guest. Uma barreira no command buffer atual não
substitui dependências entre filas.

Referência oficial consultada via Context7:
[exemplos de sincronização e ownership](https://github.com/KhronosGroup/Vulkan-Docs/wiki/Synchronization-Examples).

O estudo antigo em `docs/perf-research/raw-findings.md` já apontava esses custos.
Sua observação de utilização GPU de 40% pertence ao perfil histórico; não foi
reutilizada como medição da build atual, nem como prova de que async compute
jamais ajudaria. O Crash continua adiado pelo usuário.

## Validação feita

- CTests `guest_sync_direct`, `guest_sync_inline`, `guest_sync_threaded` e
  `guest_sync_completion`: **4/4 passaram**, com sync validation, zero erros
  e os avisos de configuração da layer e ICD DZN ignorado pelo loader.
- Esses testes exercitam a ordenação existente e limites PM4/labels. Não
  validam execução simultânea em múltiplas filas Vulkan.
- `--upload-dma-only` com `KYTY_UPLOAD_DMA=1`: **skipped**, pois não houve fila
  de transferência elegível. Exit 0 não foi contabilizado como teste DMA passado.

Logs em `_Build/async-compute-20261009/guest-sync.log` e `upload-dma.log`.

## Decisão e condição para retomar

Manter a arquitetura atual nesta entrega. Não foi demonstrado novo gargalo GPU,
ganho antes/depois, FPS, occupancy, tempo de dispatch ou impacto visual; não
houve mudança de comportamento para avaliar ou reverter.

Retomar após captura representativa mostrar dois trabalhos independentes,
tempo GPU que possa se sobrepor e custo de sincronização inferior ao ganho.
Escolher primeiro um único workload com entradas/saídas e lifetime explícitos;
comparar modo serial/paralelo com timestamps, readbacks e validação de
sincronização. Migrar todas as filas guest de uma vez não é uma otimização
focada sustentada pelos dados atuais.

A próxima frente da ordem é RT em software.
