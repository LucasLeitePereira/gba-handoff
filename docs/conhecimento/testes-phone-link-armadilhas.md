---
titulo: Armadilhas dos testes do motor do celular (comparar states, save da ROM de teste, TSan)
tipo: armadilha
gerado_por: claude-sonnet-5-5
commit: d7240e6
arquivos: tests/phone_link_test.c, tests/phone_sim.h
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T17:57:30Z
parecer: Os quatro primeiros itens são iguais aos da versão aprovada. Já foram conferidos antes: a máscara [304,496) e [880,976) no sim, sav_const/sav_bias, 600000 para desligar o autosave e phone_link_test_san só com ASan/UBSan.; O item das teclas foi corrigido e confere com o código. Em tests/roms/gbah_test.s, fora da variante LATE, a ROM grava SRAM[1] = teclas & 0xFF e SRAM[2] = teclas >> 8 (`strb r6, [r1, #2]`). Em tests/phone_link_test.c:600-663, o cenário H envia H_KEYS | H_LR e verifica `sav_byte_at(..., 2) == 3`, tanto no celular depois do RETURN (modo 1) quanto no PC (modo 3). A afirmação anterior de que R e L não podiam ser verificados estava desatualizada, e esta versão a substitui. A referência a rom-de-teste-gitignore é coerente: aquele documento tirou a parte da SRAM de propósito e deixou a correção para este.; O item novo (cenário P) confere. Em scenario_p (phone_link_test.c:381-445), o teste grava na pasta do PC uma meta com `timestamp_ms = now + 60000` e sobe o PC com a ROM como argumento. No modo 2, chama `sim_pause` antes de conectar (comentário: 'the connect-time save is not newer than the PC's') e confere o vencedor por `aft.version.dev != ph.dev` (comentário: 'the PC advanced on open: its own device id'). Em desktop/handoff.cpp:120-121, take_snapshot chama gbah_sync_advance com gbah_now_ms(), e sync/gbah_sync.c:40-48 sobrescreve timestamp_ms e version.dev. Por isso o timestamp que o teste plantou se perde, como o documento diz. O modo 1 também é coerente: o celular vence mesmo com o PC tendo plantado um timestamp no futuro.; O conhecimento é novo e não duplica nada. Nenhum documento aprovado explica que o PC regrava a meta ao abrir e que isso invalida um timestamp plantado pelo teste. A cópia aprovada atual estava corrompida, com linhas duplicadas e texto cortado, e esta versão fica limpa.; O texto explica armadilhas e o porquê (sintoma, causa e como o teste contorna) e não resume o código. Continua conciso e não tem segredos nem dados pessoais.
---

# Armadilhas dos testes do motor do celular

- **Comparar states do mGBA: ignorar dois trechos.** O `sim` compara o state inteiro byte a byte,
  exceto o bloco de áudio `[304,496)` e os buffers de amostras `[880,976)`
  (`offsetof(GBASerializedState, audio / samples..reserved)`). O resíduo ali difere entre um core
  que carregou um state e um que rodou até o mesmo ponto, mesmo com o jogo idêntico. Sintoma sem a
  máscara: "states diferentes" em todo teste de restore. CPU, timers, DMA, IO, VRAM e RAM têm de
  bater.
- **Não use hash de framebuffer como prova de estado.** A ROM de teste tem tela estática, então dois
  estados distintos dão o mesmo hash. Por isso a comparação é do state inteiro.
- **A ROM de teste regrava o `.sav` a cada frame.** Com o hash real, o motor vê "o jogo gravou"
  o tempo todo e o autosave por mudança de `.sav` dispara sem parar (no limite de ~10 s, ver
  android-motor-phone-link). Por isso o cenário que testa esse gatilho liga `sav_const` (hash
  constante) e simula a gravação somando `sav_bias`. Os tempos (`autosave_ms`, `sav_min_ms`,
  `tick_ms`) são escolhidos por cenário: curtos para exercitar o autosave, 600000 para desligá-lo.
  Não são os valores de produção.
- **TSan não rodou neste ambiente** (observado no WSL2: "unexpected memory mapping"; não há alvo
  TSan no CMake). A cobertura de threads é só ASan/UBSan (`phone_link_test_san`, com as mesmas
  armadilhas de largada do ASan de testes-armadilhas-fase4). Corridas de dados no motor só são
  pegas revisando os locks.
- **Provar quais teclas o PC aplicou, sem gancho de teste no PC.** A ROM de teste grava as teclas a cada frame em `SRAM[1]` (bits 0-7) e `SRAM[2]` (bits 8-9: L e R); o `.sav` que volta no RETURN (ou o do próprio PC) mostra o que foi aplicado. Um `.gba` antigo, sem a gravação de `SRAM[2]`, deixa L/R sem prova: depois de regravar a ROM (ver rom-de-teste-gitignore) a expectativa do teste é `SRAM[2] == 3` para L+R.
- **No modo "mais recente" o PC regrava o `meta` ao abrir o jogo.** O cenário P sobe o PC já com o jogo aberto (argumento da ROM) depois de o teste gravar na pasta dele uma meta com `timestamp_ms` no futuro. O PC avança a versão ao abrir, com a hora atual, e o timestamp plantado se perde: a hora que decide o conflito não é a do teste. Por isso o cenário usa `sim_pause` no celular (modo 2) para o salvamento da conexão não ser mais novo que o do PC, e confere o vencedor pelo id do aparelho na meta final, não pelo timestamp que o teste escreveu.
