---
titulo: Decisões do app Android (UI, ritmo, vídeo, save) e limitações atuais
tipo: decisao
gerado_por: claude-sonnet-5-5
commit: sem-commit
substitui: conhecimento/android-decisoes-app.md
arquivos: android/app/src/main/cpp/jni_bridge.c, android/app/src/main/java/io/gbahandoff/GameActivity.kt, android/app/src/main/java/io/gbahandoff/SaveStore.kt, android/app/src/main/java/io/gbahandoff/MainActivity.kt
motivos_rejeicao: O texto é igual à versão aprovada (conhecimento/android-decisoes-app.md), exceto pela nova seção 'Validação no aparelho (Fase 2, relato do usuário que testou)'. Só essa seção precisa ser avaliada.; Essa seção é um relatório de teste: áudio correto, velocidade normal e retomada no ponto exato. Ela não registra uma decisão, uma alternativa descartada nem uma armadilha com sintoma. Diz que o que já estava documentado funcionou, e isso não ajuda quem lê o código no futuro a entender por que ele é assim (item 3 do checklist).; O relato não pode ser conferido no repositório: não há teste, log nem registro da 'Fase 2' que o sustente. Também deixa de valer na próxima mudança no ritmo ou no save, e o documento não diz em qual commit ou build o teste foi feito (o campo commit está como 'sem-commit').; Tirando essa seção, o resto é igual ao documento aprovado, e substituí-lo não traz conhecimento novo (item 2 do checklist).; O que o autor precisa mudar: retirar a seção de validação. Se o teste no aparelho revelou algo que não está documentado, como um sintoma, um ajuste necessário ou uma condição em que falha, registre isso como armadilha, com a causa e onde aparece no código. Um status de progresso não deve entrar na base de conhecimento.
tentativas: 1
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T01:58:08Z
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

## Validação no aparelho (Fase 2, relato do usuário que testou)
Áudio correto, velocidade normal e retomada por state+`.sav` voltando exatamente ao ponto. Isso valida na prática o
ritmo por escrita AAudio bloqueante e o save no `onPause`.

## Limitações atuais (não são decisões; vão mudar)
- O `.sav` só vai para o disco no `onPause`. Se o jogo salvar in-game e o processo morrer sem passar
  por `onPause` (kill, crash, bateria), esse progresso se perde, e a retomada usa o state anterior.
- O app guarda só a URI da ROM (`last_rom` nas preferências), não os bytes. A transferência da ROM no
  handoff vai precisar ler os bytes pela URI, e a permissão persistível pode não existir em alguns
  providers (o `SecurityException` é ignorado e a URI só vale naquela sessão).
