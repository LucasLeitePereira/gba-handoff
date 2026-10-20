---
titulo: Decisões do app Android (UI, ritmo, vídeo, save)
tipo: decisao
gerado_por: claude-sonnet-5-5
commit: 50fe505
arquivos: android/app/src/main/cpp/jni_bridge.c, android/app/src/main/cpp/phone_link.c, android/app/src/main/java/io/gbahandoff/GameActivity.kt, android/app/src/main/java/io/gbahandoff/MainActivity.kt, android/app/src/main/java/io/gbahandoff/LinkClient.kt
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T13:41:49Z
parecer: Conferi no código e as afirmações batem. gradle.properties:7 tem android.useAndroidX=false. GameActivity.kt:125 chama setFixedSize. O onPause (GameActivity.kt:167) chama Native.saveNow(), e saveNow vai para phone_save_now, que chama checkpoint(p, 0, NULL). checkpoint devolve 1 sem gravar quando o frame não mudou desde a última gravação ou quando o jogo está no PC ou com handoff pendente.; O autosave está em save_main (phone_link.c:293). Grava a cada 30 s por padrão (autosave_ms) e quando o sav_hash muda. A gravação passa por gbah_store_commit, que escreve state, .sav e .meta com gbah_fs_write_atomic (.tmp + rename). O diretório é filesDir/games (LinkClient.kt:60). RomLibrary usa roms.tsv e files/roms, e a ROM escolhida pelo usuário vence a recebida (RomLibrary.kt:15). O SecurityException de takePersistableUriPermission é engolido (MainActivity.kt:146). phone_resume_stored aplica o state pelo host, e core_host.c:57 usa gbah_handoff_restore.; Correção: a retomada não usa o .sav sozinho apenas quando não há state. Se o state existe mas apply falha, phone_resume_stored faz cold_sav com o .sav (phone_link.c:1490-1494). Recoloquei 'ou se ele falhar ao carregar', que a versão aprovada tinha e este texto perdeu.; Não há duplicação. O doc substitui a versão aprovada, que ficou desatualizada (falava de save só no onPause, SaveStore e last_rom). A cadência e o mecanismo do autosave ficam no pendente android-motor-phone-link, que este só referencia. Também não há segredos nem dados pessoais.
---

# Decisões do app Android

- **Views simples, sem androidx/Compose** (`android.useAndroidX=false`). Motivo: leveza (menos
  dependências). Custo aceito: UI feita à mão.
- **Ritmo da emulação = escrita AAudio bloqueante.** O relógio do dispositivo de áudio dita o passo,
  como a fila de áudio no desktop; evita deriva entre som e vídeo sem lógica de sincronização. Sem
  áudio (falha ao abrir, ou erro de escrita seguido de falha ao reabrir), cai para um timer de
  59,7275 Hz.
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
