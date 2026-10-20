---
titulo: Armadilhas dos testes do motor do celular (comparar states, save da ROM de teste, TSan)
tipo: armadilha
gerado_por: claude-sonnet-5-5
commit: 67beb79
arquivos: tests/phone_link_test.c, tests/phone_sim.h
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T14:27:53Z
parecer: Os quatro itens que já estavam aprovados continuam iguais. Já foram conferidos na revisão anterior (phone_sim.h compara o state inteiro e ignora [304,496) e [880,976); sav_const/sav_bias; 600000 para desligar o autosave; phone_link_test_san só com ASan/UBSan). Esta versão também corrige a cópia aprovada, que estava corrompida (linhas duplicadas e texto cortado).; O item novo (provar quais teclas o PC aplicou) confere com o código. Em tests/roms/gbah_test.s:42-44 e 62-63, a ROM lê KEYINPUT, inverte, fica com 10 bits e grava com `strb r5, [r1, #1]`, ou seja, só o byte baixo vai para SRAM[1] a cada frame. Em tests/phone_link_test.c:449-505, o cenário H lê `.sav[1]` do celular depois do RETURN (modos 0-2) e do próprio PC (modos 3-4). Em core_test.c:96, a verificação é `INPUT & 0xFF`. Como o byte gravado é só o baixo, R e L (bits 8-9) de fato não podem ser verificados assim.; O conhecimento é novo e não duplica nada: nenhum documento aprovado explica como verificar, sem gancho de teste no PC, quais teclas o PC aplicou, nem o limite de um byte. contrato-handoff-estado só define a ordem dos bits.; O texto explica o porquê (não há gancho no PC, então o .sav serve de canal de observação) e a armadilha (R e L ficam de fora), em vez de resumir o código. É conciso e não tem segredos nem dados pessoais.
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
- **Provar quais teclas o PC aplicou, sem gancho de teste no PC.** A ROM de teste grava as teclas em `SRAM[1]` a cada frame; o `.sav` que volta no RETURN (ou o do próprio PC) mostra o que foi aplicado (cenário H). Armadilha: é um byte só (`keys & 0xFF`, como em `core_test.c`), então só cobre A, B, Select, Start e as quatro direções; R e L (bits 8-9) não são verificáveis assim.
