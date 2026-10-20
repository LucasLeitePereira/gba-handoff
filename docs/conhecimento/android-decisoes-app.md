---
titulo: Decisões do app Android (UI, ritmo, vídeo, save) e limitações atuais
tipo: decisao
gerado_por: claude-sonnet-5-5
commit: sem-commit
arquivos: android/app/src/main/cpp/jni_bridge.c, android/app/src/main/java/io/gbahandoff/GameActivity.kt, android/app/src/main/java/io/gbahandoff/SaveStore.kt, android/app/src/main/java/io/gbahandoff/MainActivity.kt
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T01:34:40Z
parecer: Conferi as afirmações no código e quase todas batem. gradle.properties:7 tem android.useAndroidX=false. Em jni_bridge.c, emu_main faz a escrita AAudio bloqueante fora do lock e, quando audio_write devolve -1 ou não há stream, cai para o timer de 59,7275 Hz (GBA_FPS). Se a reabertura falha em audio_write, o resultado também é -1. layoutScreen em GameActivity.kt calcula o maior k inteiro e chama setFixedSize(240k, 160k). setSurface usa setBuffersGeometry(0,0) para manter esse tamanho. O onPause grava state e .sav por SaveStore (filesDir/games/<sha1>.state/.sav), e write_file_atomic faz tmp + rename. resume() tenta restoreFiles (gbah_handoff_restore) primeiro e só usa loadSavFile se não houver state ou se ele falhar. MainActivity guarda só a URI em last_rom, e takePersistableUriPermission pode lançar SecurityException, que o código engole.; Corrigi `ROM_PUSH`: esse nome não existe em nenhum lugar do código nem dos docs, fora este pendente. Troquei por 'a transferência da ROM no handoff'.; Tirei a afirmação 'APK de poucos MB / build mais barato', que não dá para conferir no repositório. Deixei só o motivo (leveza, PC/celular modestos) e o custo aceito.; Deixei a limitação 1 mais precisa. O .sav só é gravado no onPause, então o save in-game feito depois do último onPause nunca chega ao disco. Não é só que a retomada prefira o state.; Não há duplicação. mgba-integracao-armadilhas cobre o ritmo do desktop (fila SDL), e android-build-armadilhas cobre build e AAudio setUsage. As decisões de runtime do Android são novas. Também não há segredos nem dados pessoais.
---

# Decisões do app Android e limitações atuais

- **Views simples, sem androidx/Compose** (`android.useAndroidX=false`). Motivo: leveza (menos
  dependências, APK e build menores). Custo aceito: UI feita à mão.
- **Ritmo da emulação = escrita AAudio bloqueante.** O relógio do dispositivo de áudio dita o passo,
  como a fila de áudio no desktop; evita deriva entre som e vídeo sem lógica de sincronização. Sem
  áudio (falha ao abrir, ou erro de escrita seguido de falha ao reabrir), cai para um timer de
  59,7275 Hz.
- **Vídeo direto na `ANativeWindow`**, sem GL. A `SurfaceView` recebe `setFixedSize(240k, 160k)` (maior
  k inteiro que cabe na área; em retrato, a metade de cima), então 1 pixel do buffer = 1 pixel da tela:
  o compositor não reescala nem borra o pixel art. Alternativa descartada: escalar por GL/filtro.
- **Save automático no `onPause`**: grava `filesDir/games/<sha1>.state` e `.sav`, chaveados pelo
  SHA-1 da ROM, com escrita atômica (`.tmp` + `rename`) para um crash no meio não corromper o save
  anterior.
- **Retomada sempre pelo state** (`gbah_handoff_restore`); o `.sav` sozinho só é usado se não houver
  state ou se ele falhar (ver contrato-handoff-estado).

## Limitações atuais (não são decisões; vão mudar)
- O `.sav` só vai para o disco no `onPause`. Se o jogo salvar in-game e o processo morrer sem passar
  por `onPause` (kill, crash, bateria), esse progresso se perde, e a retomada usa o state anterior.
- O app guarda só a URI da ROM (`last_rom` nas preferências), não os bytes. A transferência da ROM no
  handoff vai precisar ler os bytes pela URI, e a permissão persistível pode não existir em alguns
  providers (o `SecurityException` é ignorado e a URI só vale naquela sessão).
