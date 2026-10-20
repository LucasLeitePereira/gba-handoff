---
titulo: Decisões do app Android (UI, ritmo, áudio, vídeo, save)
tipo: decisao
gerado_por: claude-sonnet-5-5
commit: 67beb79
arquivos: android/app/src/main/cpp/jni_bridge.c, android/app/src/main/java/io/gbahandoff/GameActivity.kt, android/app/src/main/java/io/gbahandoff/LinkUi.kt, android/app/src/main/java/io/gbahandoff/Placement.kt, android/app/src/main/java/io/gbahandoff/PadView.kt, android/app/src/main/cpp/phone_link.c, android/app/src/main/java/io/gbahandoff/MainActivity.kt, android/app/src/main/java/io/gbahandoff/LinkClient.kt
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T14:26:04Z
parecer: Conferi no código as duas partes novas e as afirmações batem. Áudio: em emu_main (jni_bridge.c:232-251), o stream fecha ao entrar em hold, com o log 'audio closed (hold)'. Ao sair do hold ele reabre fora do g.mtx, com o log 'audio reopened (hold ended)', e o comentário da linha 232 confirma o motivo do lock. O audio_error_cb (jni_bridge.c:112-117) só faz atomic_store(&g.audio_lost, 1). O audio_write (jni_bridge.c:184-200) reabre quando o estado não é STARTED nem STARTING, quando a escrita dá r < 0 ou quando dá timeout de 100 ms (100*1000000 ns). Se a reabertura falha, retry = 120 frames, que dá ~2 s, e a retentativa registra 'audio reopened (retry)'. Os logs 'audio closed (%s)' e 'audio reopened (%s)' existem em audio_reopen.; Overlay: o texto 'No PC • controle ativo' está em LinkUi.kt:55. O container tem isClickable = false (LinkUi.kt:45), e o chip é 'Voltar ao celular'. Placement.pick tenta os cantos e, se todos colidem, fica com o de menor área sobreposta. PadView.occupiedRects inclui os controles com folga de 8dp e a engrenagem com folga de 6dp. placeFloating roda em refreshLink, chamado pelo poll de POLL_MS = 300 (GameActivity.kt:38, 105, 211). A ordem z bate com o texto: padHost entra antes de sendBtn e onPcOverlay em onCreate, e PadView.playTouch devolve true.; O doc mantém a correção do parecer anterior ('ou se ele falhar ao carregar'). O resto é idêntico à versão aprovada, já conferida em 50fe505. O texto sobre fallback de áudio foi generalizado de um jeito coerente com o código (falha ao abrir ou ao reabrir).; Não há duplicação. mgba-integracao-armadilhas fala do ritmo no desktop e do áudio mudo por config, não do ciclo de vida do stream AAudio no hold nem da pílula do overlay. Este doc substitui a própria versão anterior.; Explica o porquê, não só o que o código faz. Para o áudio traz sintoma, causa marcada como inferida e como verificar no logcat, além das armadilhas do lock, do callback e do STARTING. Para o overlay explica por que é pílula e por que o pad fica abaixo na ordem z. Não há segredos nem dados pessoais. A seção de áudio é longa, mas cada regra é uma armadilha real que não se deduz lendo o código.
---

# Decisões do app Android

- **Views simples, sem androidx/Compose** (`android.useAndroidX=false`). Motivo: leveza (menos
  dependências). Custo aceito: UI feita à mão.
- **Ritmo da emulação = escrita AAudio bloqueante.** O relógio do dispositivo de áudio dita o passo,
  como a fila de áudio no desktop; evita deriva entre som e vídeo sem lógica de sincronização. Sem
  áudio (falha ao abrir ou ao reabrir), cai para um timer de 59,7275 Hz.
- **O stream AAudio é fechado enquanto o jogo está no PC (`hold`) e reaberto ao voltar.** Sintoma
  no aparelho: após celular -> PC -> celular o som não voltava até sair e entrar no app. Causa
  INFERIDA, não observada (faltou logcat): o stream ficava STARTED e vazio por minutos e o sistema o
  desconectava, e só erro < 0 na escrita reabria. Regras atuais: (1) fechar/abrir acontece em
  `emu_main`, FORA do `g.mtx` (abrir leva dezenas de ms e `hold()` toma esse lock); (2) o error
  callback só marca um flag atômico, porque roda em thread do sistema e fechar o stream de dentro
  dele trava ou derruba o app; (3) reabre se o estado não for STARTED nem STARTING (STARTING é normal
  logo após `requestStart`, tratá-lo como falha reabriria em laço), se a escrita der erro ou se
  estourar o timeout de 100 ms; (4) se a reabertura falha, o jogo segue no timer e tenta de novo
  a cada ~2 s. Verificar: `adb logcat -s gbah` mostra `audio closed (motivo)` e `audio reopened
  (motivo)`. Se o sintoma voltar sem esses logs, a causa inferida estava errada.
- **Vídeo direto na `ANativeWindow`**, sem GL. A `SurfaceView` recebe `setFixedSize(240k, 160k)` (maior
  k inteiro que cabe na área; em retrato, a metade de cima), então 1 pixel do buffer = 1 pixel da tela:
  o compositor não reescala nem borra o pixel art. Alternativa descartada: escalar por GL/filtro.
- **Quem salva é o motor `phone_link`, não a Activity** (ver android-motor-phone-link). O `onPause`
  chama `Native.saveNow()` (síncrono; devolve 1 e não grava se nada mudou ou se o jogo está no PC),
  mas deixou de ser o único ponto de gravação: há autosave a cada ~30 s e quando o `.sav` do
  cartucho muda. Kill ou crash sem `onPause` perde no máximo o que veio depois do último autosave,
  não mais tudo desde a última pausa.
- **Cada gravação é uma versão nova** (state + .sav + .meta, escrita atômica `.tmp` + `rename`),
  chaveada pelo SHA-1 da ROM em `filesDir/games`. Regras de versão: sync-regras-versao.
- **Retomada sempre pelo state** (`phone_resume_stored`, que usa `gbah_handoff_restore`); o `.sav`
  sozinho (partida a frio) só é usado se não houver state ou se ele falhar ao carregar (ver
  contrato-handoff-estado).
- **O app guarda a origem da ROM, não os bytes** (`RomLibrary`: URI SAF em `roms.tsv`, mais as ROMs
  recebidas do PC em `files/roms/<sha1>.gba`; a escolhida pelo usuário vence a recebida). Para enviar
  a ROM ao PC o motor lê pela origem; `content://` sem permissão persistível só vale naquela sessão
  (o `SecurityException` de `takePersistableUriPermission` é ignorado), então o envio dessa ROM falha
  depois que a permissão some.
- **Overlay "No PC • controle ativo" é uma pílula, não uma tampa.** Antes cobria a imagem do
  jogo e engolia os toques; agora, com o celular também sendo controle, o pad tem de continuar
  tocável. O contêiner não é clicável (só o chip "Voltar ao celular"). `Placement.pick` põe a pílula
  (e o chip "Enviar ao PC") no primeiro canto da imagem que não toca nenhum controle ou a engrenagem
  (com a folga de acerto), e se todos colidem, no de menor área sobreposta; é reavaliado a cada poll
  de 300 ms, então mover um controle no editor é percebido. Na ordem z o pad fica ABAIXO dos chips:
  o `PadView` consome todo toque, então um chip por baixo nunca receberia clique.
