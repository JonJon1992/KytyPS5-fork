# KytyPS5-Demon: saves e compactação de LDS

Referência: [KytyPS5-Demon f17993c1](https://github.com/JonJon1992/KytyPS5-Demon/tree/f17993c1f7af06b622887fe5c496517cd690e35c).
O usuário autorizou a integração e pediu incluir otimização nesta etapa.

## Espaço livre dos saves

`SaveDataDirNameSearch` e `SaveDataGetMountInfo` passam a descontar os bytes dos
arquivos de dados da capacidade informada, arredondando o total para blocos de
64 KiB. O diretório raiz `sce_sys` contém metadados do emulador e não entra nessa
conta. Uso igual ou superior à capacidade retorna zero livre; erros ao percorrer
o diretório retornam o erro interno existente.

O formato binário `sce_sys/blocks.bin`, os limites de alocação e os slots de
montagem existentes são preservados. O formato textual do doador não foi usado.

## Compactação de memória temporária em pixel shaders

O Kyty já representa LDS de pixel shaders como memória privada da invocação,
com um array Function de 8192 dwords. Quando todos os acessos são leituras ou
escritas escalares de 32 bits em `(LaneId << 2) + offset`, cada offset distinto
pode usar uma única posição desse array: o termo de lane é igual em todos os
acessos daquela invocação.

`FunctionLdsLayout.h` adapta a análise do doador, com duas proteções adicionais:

- Offsets são normalizados módulo 65536, conforme a máscara de endereço já
  aplicada pelo nosso emissor. Por exemplo, offsets 0 e 65536 continuam aliases.
- A compactação só ocorre quando usa menos de 8192 posições.

A análise rejeita o layout inteiro para outro estágio, metadados inválidos,
endereço incompatível ou acesso LDS subword, vetorial ou atômico. GDS permanece
separado. A mudança reduz apenas o array e os índices dos ponteiros internos;
os cálculos de endereço e as verificações contra o limite original continuam.
Não é adicionada inicialização de memória.

Ativa por padrão; `KYTY_FUNCTION_LDS_COMPACT=0` restaura o caminho anterior.
A opção participa do fingerprint do cache. O otimizador SPIR-V estendido da
integração AnyPS5 continua opcional e desligado por padrão.

### Medição dos fixtures

Comparação na RX 9070 XT, com resultados gráficos idênticos entre original e
compactado, tanto com a limpeza SPIR-V atual ligada quanto desligada:

| Fixture | Array original | Após redução de arrays já existente | Após compactação nova |
| --- | ---: | ---: | ---: |
| Duas posições, offsets 0 e 1024 | 32768 bytes | 1280 bytes | 8 bytes |
| Uma posição | 32768 bytes | 256 bytes | 4 bytes |
| Endereço misto, fallback | 32768 bytes | 2052 bytes | 2052 bytes |

A redução existente foi medida com `SpirvLocalArrays::SetMaxSubgroupSize(64)`.
Os valores são **memória declarada no SPIR-V por invocação**, não uma medição de
VRAM física do driver. O módulo bruto do fixture de duas posições passa de
455 para 459 palavras; o benefício é remover a indexação dinâmica do array e
diminuir seu tamanho, não encurtar o módulo. Não foi medido ganho de FPS.

## Decisões sobre os outros candidatos

- A análise lane-local do doador evita duplicar operações wave64 em hosts de
  largura 32. Nosso caminho AMD já seleciona wave64 nativo quando suportado.
  Além disso, o analisador do doador não rejeita automaticamente os opcodes que
  só nosso enum possui. Sua importação exigiria ampliar a prova e os testes.
- A extração estática de shaders do doador depende dos arquivos e materiais de
  Demon's Souls. Nosso precompile/prefetch/fast-first geral permanece em uso.
- Reescrever um staging upload já gravado muda os bytes vistos por draws
  anteriores. O reaproveitamento do doador depende dessa hipótese de streaming;
  não foi aplicado ao nosso controle de versões sem uma prova de sincronização.

## Validação

- RED dos saves: a busca retornava a alocação inteira mesmo com payload gravado.
- RED gráfico: o fixture de duas posições reservava 8192 dwords, em vez de 2.
- RED do analisador: a versão do doador separava offsets que dão a volta em
  64 KiB; a versão sem teto também aceitava uma alocação maior que a original.
- GREEN gráfico inicial: sete fixtures em quatro configurações (28 renderizações),
  com comparação exata, incluindo limites, wrap e fallback.
- O teste inspeciona o SPIR-V bruto para preservar as comparações contra 8192.

Build Release final concluído; os 15 testes CTest selecionados passaram. Incluem
o analisador, as opções default/on/off, comparação gráfica, cache, saves,
redução de arrays, otimizador SPIR-V e as instruções integradas do AnyPS5.
Também passaram 57 casos GPU do seletor `--ds-atomics-only`.

Essa regressão adicional revelou um problema nos fixtures
`BufferAtomicFMinExactRawGlcModes` e `BufferAtomicFMaxExactRawGlcModes`: o user
data explícito deixava o word s51 do descritor de saída zerado. A falha foi
reproduzida com a compactação desligada; os stores de observação eram descartados.
Os fixtures agora inicializam esse word como o harness padrão. A implementação
das operações atômicas não mudou, e os 57 casos passaram após a correção.

Comando do grupo CTest, com a biblioteca do toolchain local no `LD_LIBRARY_PATH`:

```sh
/usr/bin/ctest --test-dir _Build/linux-clang \
  -R '^(function_lds_.*|save_data_memory|save_data_dialog|spirv_local_arrays|spirv_optimizer(_gpu|_extended)?|program_cache|shader_integer16_ternary(_extended)?|shader_mul_hi_24)$' \
  --output-on-failure -j1
```

O executável `_Build/linux-clang/install/kyty_emulator`, usado pelo launcher,
foi atualizado e seu SHA-256 confere com o build. Backup anterior e logs estão
em `_Build/demon-port-20261009/`. Não houve execução de jogo comercial nem da
suíte completa nesta validação. Os presets do usuário foram preservados.
