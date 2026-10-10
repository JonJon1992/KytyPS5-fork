# Hardware RT / RayQuery — 2026-10-09

## Decisão desta rodada

Estudo da frente 12 do roteiro Radeon/RDNA concluído. A GPU oferece as
capacidades necessárias, mas o emulador não possui um caminho RayQuery que
possa ser habilitado por configuração. Não foi implementado um backend nem
alterado o comportamento de produção. Ganho de FPS permanece não medido.

O roteiro pede estudar hardware RT separadamente e não implementar uma
arquitetura complexa sem prova clara de benefício. A evidência disponível
justifica uma avaliação de viabilidade, não ativação em jogos.

## Hardware e código verificados

Nova consulta `vulkaninfo`, executada nesta rodada e preservada em
`_Build/hardware-rt-20261009/vulkaninfo.txt`, identificou:

| Item | Radeon física |
|---|---|
| Dispositivo | AMD Radeon RX 9070 XT (RADV GFX1201) |
| Driver | Mesa 26.2.3 |
| accelerationStructure | true |
| rayQuery | true |
| rayTracingPipeline | true |

O arquivo também lista llvmpipe; os valores acima pertencem à Radeon.
Capacidade anunciada não é prova de execução, equivalência ou desempenho de RT.

No código atual:

- `frontend/translate/Memory.cpp`, `IMAGE_BVH_INTERSECT_RAY`: produz uma operação
  IR `BvhIntersect`, com quatro resultados guest, por instrução.
- `backend/spirv/spirvEmitterBvh.cpp`: lê o nó indicado pelo shader através da
  tabela de páginas guest. Caixas retornam até quatro ponteiros de filhos,
  com ordenação opcional e crescimento de limite; triângulos retornam os
  valores esperados pelo guest. O shader guest conserva o controle do percurso.
- `presentation/window/vulkanWindow.cpp`: a criação do dispositivo não encadeia
  as features de RayQuery/acceleration structure. A busca em `src` também não
  encontrou implementação de construção de AS ou operações RayQuery.

## Por que não basta trocar a instrução

As estruturas de aceleração Vulkan são opacas e dependentes da implementação.
Devem ser construídas a partir de geometria e ter sua vida útil e sincronização
geridas pela aplicação. Um endereço de BVH guest não constitui uma AS Vulkan.
[Guia Khronos](https://docs.vulkan.org/guide/latest/extensions/ray_tracing.html),
[especificação de AS](https://docs.vulkan.org/spec/latest/chapters/accelstructures.html).

RayQuery expõe interseções candidatas/confirmadas durante o percurso. Não oferece
o contrato desta instrução guest: escolher um nó interno e obter seus quatro
filhos no formato e na ordem guest.
[Especificação de traversal](https://docs.vulkan.org/spec/latest/chapters/raytraversal.html).

Inferência para este emulador: um caminho geral precisaria reconhecer um trecho
maior do percurso guest, converter geometria/instâncias, manter uma correspondência
entre hits Vulkan e identificadores guest e preservar o código de aceitação de
hits. Isso ultrapassa uma substituição local no emissor SPIR-V.

Uma alternativa de AS pequena por nó poderia ser investigada isoladamente,
mas não elimina os custos de construção, atualização, armazenamento, consultas
e reconstrução dos quatro resultados. Não há medição que a favoreça.

## Evidência necessária antes de implementação de produção

1. Captura de uma carga que execute BVH, identificando shaders e tempo GPU do
   percurso. Não presumir que o gargalo RenderThread de Crash 4 seja RT.
2. Reconhecimento de uma variante concreta de traversal e do layout de suas
   instâncias; fallback para variantes e recursos não reconhecidos.
3. Protótipo separado, inicialmente com geometria estática, comparando hits,
   ponteiros, baricêntricas, bordas, empates, raios inválidos, máscara de lanes,
   limites e regras de aceitação com o caminho software.
4. Medição separada da conversão, build/update de BLAS/TLAS, consultas,
   sincronização e memória adicional. Incluir alterações de geometria e
   invalidação por escritas CPU/GPU antes de considerar uso dinâmico.
5. Comparação do custo total na mesma cena e qualidade, incluindo percentis de
   tempo de quadro, antes de promover o caminho para produção.

O custo a comparar é conversão + atualização de AS + consulta + sincronização,
não apenas o tempo de uma consulta com AS já construída. Estimativas antigas
de milissegundos ou multiplicadores no documento `perf-research/raytracing-plan.md`
não são medições desta rodada e não foram usadas para prometer ganho.

## Validação e limites

Consulta de capacidades concluída com exit code 0; documentação atual consultada
via Context7 e fontes oficiais Khronos. Esta etapa alterou somente documentação:
não houve build, execução de kernel RayQuery, benchmark, instalação ou commit.
Os testes software da frente anterior continuam sendo evidência apenas do
caminho software. O roteiro chegou à última frente; a validação em jogo das
otimizações anteriores permanece pendente, conforme o adiamento de Crash 4.

## Evidência posterior em jogo

A coleta posterior do Astro's Playroom sem patch identificou cinco shaders
compute contendo BVH efetivamente despachados. Os segmentos correspondentes
somaram 3,469–3,996 ms de GPU em quatro capturas com timestamps por passes.
Isso fornece candidatos e uma referência software, mas inclui todo o trabalho
desses shaders e não mede conversão ou execução de hardware RT. Ver
[ASTRO-RT-2026-10-09.md](ASTRO-RT-2026-10-09.md), seção de captura GPU posterior.
