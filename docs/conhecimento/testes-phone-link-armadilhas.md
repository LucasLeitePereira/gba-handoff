---
titulo: Armadilhas dos testes do motor do celular (comparar states, save da ROM de teste, TSan)
tipo: armadilha
gerado_por: claude-sonnet-5-5
commit: 50fe505
arquivos: tests/phone_sim.h, tests/phone_link_test.c, tests/phone_migrate_test.c, tests/CMakeLists.txt
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T13:46:06Z
parecer: Conferido no código: tests/phone_sim.h:453-459 compara o state inteiro e ignora [304,496) e [880,976). Os offsets batem com serialize.h do mGBA: áudio em 0x130-0x1EF e amostras FIFO/renderizadas em 0x370-0x3CF. O comentário diz que a tela da ROM de teste é estática e explica o resíduo.; O item da ROM de teste estava errado e não citava a armadilha principal. Os valores não são só 'reduzidos': phone_link_test.c:181 usa autosave_ms/sav_min_ms = 600000 para desligar o autosave num cenário. E como a ROM regrava o save a cada frame, o hash real do .sav muda sempre. O teste resolve isso com `sav_const` + `sav_bias` (phone_sim.h:209-247, phone_link_test.c:358), que faltava no texto. Corrigido.; Item do `.meta` removido: android-sync-armadilhas, já aprovado, cobre isso ('.meta é escrito por último... tem de esperar o .meta').; O código não comprova a falha do TSan: não há alvo TSan no CMakeLists nem registro da mensagem. Mantive o item como observação de execução, por ser plausível e útil, mas sem afirmar o que o código não mostra. phone_link_test_san existe com ASan/UBSan (CMakeLists.txt:93-103).; Não duplica mgba-integracao-armadilhas (áudio/blip ao vivo) nem contrato-handoff-estado (hash de frame ignora o byte X). Explica o porquê e o sintoma, é conciso e não tem segredos.
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
