# Protótipo isolado de hardware RT — 2026-10-10

O experimento em `_Build/hardware-rt-prototype-20261010/` executou RayQuery na
Radeon RX 9070 XT. `isa.log` contém `image_bvh8_intersect_ray` na ISA emitida.
Isso confirma uso da instrução de hardware no protótipo; não ativa hardware RT
no emulador, no Astro ou no Yōtei.

## Validação e limites

O teste converte 256 folhas sintéticas de 64 bytes, exclusivamente triângulos
kind 0, para BLAS e TLAS com uma instância identidade. Verifica 65.536 raios por
caminho e rodada contra resultados analíticos: hits, misses, primitive ID,
distância e baricêntricas. Repete após mover a geometria e atualizar BLAS/TLAS.
O teste com validação Vulkan e sincronização passou sem erros de validação.

Não usa BVH capturado do jogo, nem converte caixas, demais tipos de triângulos,
instâncias guest, transforms ou semântica de falhas de memória guest. Não valida
bordas, empates, NaNs ou equivalência com o percurso original. O comparador
software é uma busca linear sintética de triângulos, não o percurso BVH do Kyty.

## Medição sem concorrência de jogos e compiladores

`bench_when_idle.py` esperou a compilação terminar e executou três rodadas,
verificando processos antes e depois de cada execução. Evidências: `bench-status.log`,
`bench-{0,1,2}.log`, `summary.json`. Validação e dump de shaders ficaram desligados.
Tempos em microssegundos; consultas têm 30 amostras por caminho e fase, descartada
a primeira iteração de cada execução. Construção/conversão têm três amostras.

| Operação | Mediana (µs) |
| --- | ---: |
| Conversão inicial na CPU | 2,745 |
| Construção BLAS + TLAS na GPU | 231,84 |
| Conversão após mudança na CPU | 0,601 |
| Atualização BLAS + TLAS na GPU | 195,72 |
| Consulta hardware, geometria inicial | 127,70 |
| Busca linear software, geometria inicial | 242,32 |
| Consulta hardware, geometria atualizada | 129,46 |
| Busca linear software, geometria atualizada | 271,50 |

Timestamps GPU não incluem submissão/espera/readback no host. A consulta isolada
foi mais rápida que a busca linear; somar construção ou atualização elimina a
vantagem neste cenário. A soma de medianas é apenas ilustrativa, não uma medição
end-to-end. Reutilização de estruturas precisa ser investigada antes da integração.
Não há ganho de FPS demonstrado em jogos. Astro e Yōtei continuam pendentes para
validação de um backend integrado.

## Código de RT do AnyPS5

Revisão estática do main fixado em
`c963432db05f8b35c62e212b3a36494501522fe2`, obtido em 2026-10-10:

- `core/shader/recompiler/SpirvBackend/src/SpirvImageEmitter.cpp`,
  `EmitImageBvhIntersectRay`: interseções software em SPIR-V, com BVH64 e
  direções/inversas A16. Não foi encontrado backend RayQuery/AS Vulkan no código
  pesquisado. O modo miss opcional não representa o caminho normal do emissor.
- `core/libs/prx/libSceAgcDriver/tests/execution/Bvh64IntersectRay.cpp`:
  fixtures para quatro formas BVH32/BVH64 com e sem A16, caixas e triângulos.
- Nosso `ShaderRecompiler.cpp` ainda marca BVH64/A16 como formas não suportadas
  e pula os dispatches/draws cujos shaders as contêm. Há oportunidade concreta de
  portar suporte e adaptar testes para melhorar compatibilidade.
- Evitar substituir o emissor inteiro: AnyPS5 seleciona triângulos kind 0/1;
  o nosso já contempla kind 0/1/2/3. Endereçamento, resultados e faults precisam
  continuar seguindo os contratos locais.

Os testes do doador foram inspecionados, não executados nesta revisão. Nenhum
código de RT do AnyPS5 foi importado nesta etapa. BVH64/A16 são candidatos de
compatibilidade, sem ganho de desempenho comprovado e sem evidência de que sejam
as variantes usadas pelos shaders medidos no Astro.

Atualização posterior: o suporte BVH64/A16 foi implementado no recompilador
local e validado em CPU/GPU; ver [relatório da implementação](BVH64-A16-2026-10-10.md).
