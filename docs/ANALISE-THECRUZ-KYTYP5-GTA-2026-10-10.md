# TheCruZ/KytyPS5-GTA — análise de utilidade (2026-10-10)

## Resultado

Este fork é uma boa fonte de casos reproduzíveis e implementações focadas em GPU, descritores e shaders. O ponto mais útil para o trabalho atual é o suporte a escritas por V# escolhido em runtime. O código do doador oferece uma implementação e testes comparativos, mas sua finalização espera a GPU depois de cada dispatch; esse custo entra em conflito com a arquitetura local de settle adiado. Também vale estudar o uso de tabelas T# indexadas pela GPU em arrays bindless.

As otimizações de BDA, tabelas de recursos e redução de custo por draw têm forte sobreposição com mudanças locais em andamento. O fork deve servir como referência por commit, sem substituir os caminhos locais de coerência, sequenciamento, DrawPrep ou cache.

## Estado do repositório

O projeto é um fork de KytyPS5 focado em GTA V para PS5, código de jogo `PPSA04263`. A branch `main` termina em `f24e5df9` (2026-10-10). O README relata 53 commits sobre a base upstream sincronizada em 2026-10-08, 11 correções propostas pelo fork já incorporadas ao upstream e outras seis corrigidas independentemente. No grafo observado em 2026-10-10, a `main` oficial de KytyPS5 está 56 commits à frente e este fork tem 53 commits que não estão na `main` oficial. Esses números medem ancestralidade; commits equivalentes podem ter sido reaplicados com outros hashes. Portanto, confira o estado do upstream e o código local antes de portar qualquer mudança.

Além de `main`, o clone contém 22 referências `pr/*`, preservadas por tópico. As mais relevantes para o nosso trabalho são [`pr/vsharp-table-stores`](https://github.com/TheCruZ/KytyPS5-GTA/tree/pr/vsharp-table-stores), [`pr/bindless-image-tables`](https://github.com/TheCruZ/KytyPS5-GTA/tree/pr/bindless-image-tables), [`pr/scalar-read-before-own-write`](https://github.com/TheCruZ/KytyPS5-GTA/tree/pr/scalar-read-before-own-write), [`pr/depth-copy-no-feedback-loop`](https://github.com/TheCruZ/KytyPS5-GTA/tree/pr/depth-copy-no-feedback-loop), [`pr/layered-depth-ranges`](https://github.com/TheCruZ/KytyPS5-GTA/tree/pr/layered-depth-ranges) e [`pr/msaa-resolve-layer`](https://github.com/TheCruZ/KytyPS5-GTA/tree/pr/msaa-resolve-layer). Os nomes `pr/*` não indicam, por si só, que essas propostas continuem abertas.

O README atribui à configuração Performance 45–60 FPS em áreas urbanas e 60 FPS em áreas abertas, num RTX 3090 e Ryzen 9 5900X, contra cerca de 12–13 FPS antes das otimizações descritas. É uma referência de workload e de resultado relatado pelo autor; não prevê o desempenho numa RX 9070 XT/RADV nem em outros jogos.

## Candidatos relevantes

| Implementação doadora | Utilidade para o fork | Sobreposição e limite |
| --- | --- | --- |
| [Escritas compute por V# de uma tabela de descritores](https://github.com/TheCruZ/KytyPS5-GTA/commit/a9bfd8dd) | Referência direta para shaders que escolhem V# em runtime e escrevem por ele, como os kernels do Yōtei. Inclui rastreamento das páginas escritas pela GPU e casos de teste. | O caminho doador faz submit e espera a GPU para assentar os resultados após cada dispatch. A arquitetura local já trata esse settle como trabalho que não deve bloquear a thread GPU. Estude a análise de endereços, o bitmap e os testes; não transporte essa espera por dispatch nem substitua o settle local. |
| [Tabelas T# indexadas pela GPU como arrays de imagens](https://github.com/TheCruZ/KytyPS5-GTA/commit/fb67c552) | Pode ajudar shaders que selecionam texturas durante a execução, especialmente heaps grandes usados por ray tracing e materiais. | O fork local já tem bindless, copy-on-write e memoização de imagens. Compare layout, indexação não uniforme, limites do dispositivo, flags de descritor e lifetime das views antes de decidir se há uma lacuna real. |
| [Sincronização BDA por páginas CPU-dirty](https://github.com/TheCruZ/KytyPS5-GTA/commit/124b267c), [uma batch por submissão](https://github.com/TheCruZ/KytyPS5-GTA/commit/9867f5d1) e [uploads limitados ao que o shader pode alcançar](https://github.com/TheCruZ/KytyPS5-GTA/commit/21894f2b) | Bons pontos de comparação para reduzir scans e cópias de memória guest em workloads com muitos dispatches. | A árvore local já contém `KYTY_BDA_SYNC_PER_SUBMISSION` com modo de verificação e trabalho adicional em dirty runs, hot pages e batches. O commit de batching do doador envolve as cópias com barreiras globais; qualquer comparação deve preservar a ordenação, as escritas do CP e os recursos em voo. Não é uma otimização nova para portar sem medir. |
| [Avaliação de recursos por grafo compilado](https://github.com/TheCruZ/KytyPS5-GTA/commit/0a10e28c) e [atualização bindless só para slots alterados](https://github.com/TheCruZ/KytyPS5-GTA/commit/4e0d0d01) | Ajudam a comparar o custo de resolver SRTs e publicar tabelas de imagens. O README relata avaliação de SRT cerca de 3,8× mais rápida no workload do autor. | O fork local já tem `SrtWalker`, memoização de bindings e publicação bindless incremental. Compare invalidação, fontes guest e contadores; o multiplicador do README não é uma medição do nosso workload. |
| [Pipeline de threads para a GPU emulada](https://github.com/TheCruZ/KytyPS5-GTA/commit/2a3e684e), [redução de custos por draw](https://github.com/TheCruZ/KytyPS5-GTA/commit/3f05b63c) e [menos readbacks e esperas](https://github.com/TheCruZ/KytyPS5-GTA/commit/269c9e96) | Material de desenho para identificar filas de trabalho, cópias e sincronizações que limitam GTA V. | São mudanças grandes em command processor, recorder, scheduler, buffers e memória. Nosso fork já tem sequenciador, recorder, workers de DrawPrep e trabalho próprio de coerência. Use os commits para comparação de decisões e medições; não os aplique como conjunto. |

## Compatibilidade gráfica e shader

O suporte a ray tracing deste fork roda os dispatches de GTA V em shaders compute nos núcleos shader comuns da GPU; não é execução por hardware RT do host. É útil como workload de validação da interseção BVH por software. O fork local já suporta BVH64/A16 por software e mantém o protótipo de hardware RT separado.

Outros commits de compatibilidade podem servir quando houver um sintoma correspondente: [fallback de cópia para depth feedback](https://github.com/TheCruZ/KytyPS5-GTA/commit/4bb95768), [resolução MSAA para a camada correta](https://github.com/TheCruZ/KytyPS5-GTA/commit/3a0287c5), [queries de oclusão nativas](https://github.com/TheCruZ/KytyPS5-GTA/commit/d7af94be) e [tesselação dos troncos](https://github.com/TheCruZ/KytyPS5-GTA/commit/7d85425b). O código local já contém queries nativas, tesselação e uso de attachment feedback loop; esses commits são referências para comparar fallback, camadas e semântica do jogo, não funcionalidades automaticamente ausentes.

As branches de instruções AMD também são referências pontuais: saturação de conversões packed, valores inline FP64, offsets de amostragem, comparações inteiras de 64 bits e stores R11G11B10. Verifique decoder, emissor e testes locais para cada opcode antes de importar; parte das correções do próprio fork já foi integrada upstream e o estado local pode conter equivalentes com hashes diferentes.

## Prioridade prática

1. Para Yōtei, comparar `a9bfd8dd` e `pr/vsharp-table-stores` com a implementação local de escritas BDA adiadas, focando em cobertura de V# de runtime e casos-limite. Preservar a regra de não esperar a GPU após cada dispatch.
2. Comparar `fb67c552` com o caminho bindless local somente se ainda houver shaders T# indexados pela GPU sem tradução correta ou com heaps grandes.
3. Usar `124b267c`, `9867f5d1`, `21894f2b`, `0a10e28c` e `4e0d0d01` como referências para instrumentação e casos de teste; as famílias de otimização já estão presentes ou em desenvolvimento no worktree.
4. Recorrer às branches de depth/MSAA/occlusion/tesselação quando um jogo ou driver reproduzir precisamente esse comportamento.

Para qualquer candidato de desempenho, repetir A/B na mesma cena, build e hardware, coletando também espera de CPU, submissões, bytes copiados e tempo de GPU. Os resultados do README são específicos ao GTA V em RTX 3090/Ryzen 5900X e não devem ser tratados como expectativa para a Radeon local.
