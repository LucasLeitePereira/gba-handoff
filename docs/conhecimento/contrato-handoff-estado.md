---
titulo: Contrato do handoff - state + SRAM, ordem de restore, formatos
tipo: armadilha
gerado_por: claude-sonnet-5-5
commit: sem-commit
arquivos: core/gbah_core.c, core/gbah_core.h, tests/core_test.c, tests/roms/gbah_test_late.gba, desktop/main.cpp
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T01:35:15Z
parecer: Conferi o documento com core/gbah_core.c, core/gbah_core.h, tests/core_test.c, tests/roms/gbah_test.s, o README de tests/roms, desktop/main.cpp e o savedata.c do mGBA em third_party. Estas afirmações estão corretas: gbah_handoff_restore carrega o state e depois importa o .sav. Um .sav não nulo que falha no import vira erro. gbah_sram_import devolve GBAH_ERR_SAVE_TYPE_UNKNOWN (-3) quando o clone tem tamanho 0. gbah_load_sav_buffer usa loadSave e é o que o desktop chama hoje. gbah_state_load devolve -1 quando o tamanho está errado. Também estão certos o tipo AUTODETECT em GBASavedataLoad, o ForceType em GBASavedataDeserialize, a ROM LATE sem SRAM_V113 que grava no laço 20 e o controle negativo que exige divergência.; Corrigi uma frase. O documento diz que importar o .sav antes do state só 'funciona quando o jogo já gravou antes do reset'. Num core novo, o jogo não grava nada antes do import. O que mascara o bug é o tipo de save já ser conhecido logo depois de carregar a ROM (a string SRAM_V113 na ROM normal) e ser igual ao tipo que vem no state.; Este documento substitui o contrato aprovado e acrescenta coisas novas: a função única de restore, o erro -3, a partida a frio com gbah_load_sav_buffer e por que a ROM normal esconde o bug. Os itens de formato são os mesmos do documento anterior, como se espera de uma substituição. O texto explica os porquês, o tamanho está adequado e não há segredos nem dados pessoais.
---

# Contrato do handoff: state + SRAM, ordem de restore, formatos

- **O savestate do mGBA não contém a SRAM do cartucho.** O handoff envia state + .sav juntos.
  Sem o .sav, o jogo continua, mas o save in-game some ou volta a uma versão antiga.
- **Retomar = `gbah_handoff_restore`**: `gbah_state_load` primeiro, `.sav` depois. A ordem é imposta
  pela função; não monte a sequência na mão. Motivo, lendo o mGBA: num core novo o tipo de save é
  AUTODETECT e `GBASavedataLoad` não faz nada e devolve sucesso (`savedata.c`); depois,
  `GBASavedataDeserialize` força o tipo do state e, se diferir do atual, reinicializa a savedata,
  descartando o que foi importado antes. Importar o .sav antes do state só "funciona" quando o tipo
  de save já é conhecido logo após carregar a ROM (ex.: string `SRAM_V113` na ROM) e é igual ao do
  state; por isso o bug passou despercebido.
- **`gbah_sram_import` num core que ainda não detectou o tipo devolve `GBAH_ERR_SAVE_TYPE_UNKNOWN`
  (-3)**, não sucesso falso. Um .sav não nulo que não pôde ser importado é erro no restore.
- **Partida a frio com .sav (sem state)**: use `gbah_load_sav_buffer` (`loadSave` do mGBA, que também
  fixa o tipo pelo tamanho do arquivo), logo após carregar a ROM. Usar `gbah_sram_import` aqui falha
  em silêncio no mGBA; o desktop tinha esse bug (save ignorado ao abrir o jogo).
- **Compatibilidade do state**: só vale para a mesma versão do libmgba e a mesma ROM. O `loadState`
  do mGBA não recebe tamanho; quem rejeita state de tamanho errado é o `gbah_state_load` (-1).
- **`gbah_sram_export` devolve 0 bytes** enquanto o tipo de save não foi detectado (o jogo ainda não
  gravou). Não é erro: trate como "sem .sav".
- **Input**: bits na ordem do KEYINPUT do GBA (A,B,Select,Start,Right,Left,Up,Down,R,L = bits 0..9).
  Decisão: o protocolo de rede usa a mesma ordem.
- **Framebuffer**: XBGR8888 (bytes R,G,B,X). O mGBA deixa X em 0 ou 0xFF; no Android, forçar X=0xFF.
  O hash de frame ignora o byte X.
- **Identidade da ROM por SHA-1 próprio**, porque o mGBA só tem CRC32.
- **Teste que pega a ordem errada**: `tests/roms/gbah_test_late.gba` (variante LATE, sem a string
  `SRAM_V113`): o tipo fica indetectado e a SRAM só é gravada bem depois (laço 20 da ROM, algumas
  centenas de frames). A ROM normal já nasce com o tipo detectado e mascara o problema. O
  `core_test` tem um controle negativo permanente: aplica a ordem antiga (sav antes do state) e exige
  que o hash divirja; se um dia não divergir, o teste deixou de provar algo.
