# Comparação inicial com Jetsku int18 — 2026-10-10

Referência: [release u59-windows-20261009-int18-pre](https://github.com/Jetsku/KytyPS5/releases/tag/u59-windows-20261009-int18-pre),
commit indicado `106c9a8`. Revisão limitada às notas e aos arquivos
`Control.cpp`, `spirvEmitterFlow.cpp` e `pipelineCache.cpp` da tag, copiados para
`/tmp/kyty-jetsku-int18-review/`. Não foi feita integração de código.

## Diferenças confirmadas

- **S_WAITCNT/LDS:** o emissor local usa `OpMemoryBarrier` com escopo Workgroup.
  A int18 permite selecionar escopo e usa Subgroup por padrão. As notas ligam
  isso a device lost em shaders mesh com caminhos divergentes em AMD. É uma
  candidata de estabilidade; o mapeamento local de waves precisa ser validado
  antes de trocar o escopo.
- **LDS acima do limite:** `PipelineCache::GetComputeProgram` local ainda limita
  `lds_size_dwords` ao máximo do dispositivo. A int18 usa armazenamento em buffer
  para o excesso por padrão, preservando o clamp como opção explícita. Pode
  corrigir geometria em shaders que excedam o limite; não demonstra ganho de FPS
  e precisa de integração de bindings, limites, cache, sincronização e vida útil.
- **Pipelines RT:** as notas e `pipelineCache.cpp` mostram preparação assíncrona
  específica para pipelines RT (`KYTY_RT_PIPELINE_ASYNC`). Ainda falta comparar
  integralmente com nossos workers e a preparação de pipelines existente.

O RT anunciado nessa release é software, como explicitado nas notas. As notas
também associam parte dos ganhos em fases pesadas à desativação de consultas de
oclusão precisas, com possíveis diferenças de visibilidade. Isso não deve ser
tratado como aceleração de RT por hardware ou aplicado durante a medição atual.
Já existem soluções locais para separar waves e restringir arrays por invocação;
a presença/ausência de nomes de switches não estabelece equivalência de código.

Próxima candidata recomendada: avaliar a barreira de LDS isoladamente, com
testes de visibilidade entre lanes/subgrupos e regressões dos shaders afetados.
