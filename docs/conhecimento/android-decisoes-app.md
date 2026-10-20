---
titulo: Decisões do app Android (UI, ritmo, áudio, vídeo, save)
tipo: decisao
gerado_por: claude-sonnet-5-5
commit: 428ea59
arquivos: android/app/src/main/cpp/jni_bridge.c, android/app/src/main/java/io/gbahandoff/GameActivity.kt, android/app/src/main/java/io/gbahandoff/LinkUi.kt, android/app/src/main/java/io/gbahandoff/Placement.kt, android/app/src/main/java/io/gbahandoff/PadView.kt, android/app/src/main/cpp/phone_link.c, android/app/src/main/java/io/gbahandoff/MainActivity.kt, android/app/src/main/java/io/gbahandoff/LinkClient.kt
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T19:30:23Z
parecer: A única mudança em relação à versão aprovada (d7240e6) está no item 'A pílula e o chip ficam no canto livre'. Antes ele falava em desviar da 'engrenagem' e agora fala em desviar do 'botão de menu', que é posto primeiro. Conferi no código e a correção está certa. Em GameActivity.placeFloating (GameActivity.kt:210-224), o menuBtn é posicionado primeiro por GameMenuLogic.buttonPos. O retângulo dele, com folga `m`, entra em `blocked`, e só depois `place(onPcPill)` e `place(sendBtn)` chamam Placement.pick. A engrenagem ficou na tela inicial (MainActivity, segundo o commit 428ea59), então o texto antigo estava desatualizado.; Placement.pick (Placement.kt:25-37) devolve o primeiro canto sem colisão e, se todos colidem, o de menor área sobreposta. O poll é de 300 ms (POLL_MS = 300L), e o comentário em GameActivity.kt:199 diz que a posição é reavaliada a cada poll. A ordem z continua correta: padHost é adicionado (linha 59) antes de menuBtn, sendBtn e onPcPill (linhas 62, 66 e 70).; O resto do documento é idêntico à versão aprovada, que já foi conferida em 2026-10-20. Não há duplicação, porque o documento substitui a própria versão anterior.; Explica o porquê (decisões, alternativas descartadas, armadilha e sintoma), é conciso e não tem segredos nem dados pessoais. Revisado por claude-opus-5-5 em 2026-10-20.
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
- **"No PC" é uma tela preta cheia; o controle é opcional.** Com o jogo no PC o celular não emula e não mostra a imagem (um preto cobre a `SurfaceView`, que guarda o último quadro): só o estado ("sincronizado <hora>", ou "não sincronizado (última cópia ...)" com o cabo fora), "Voltar para o celular" e a chave "Usar como controle" (desligada por padrão). Com o cabo fora o único caminho é "Jogar aqui mesmo assim", com diálogo avisando que o PC fica com a versão dele e que um conflito pode surgir na reconexão. Ligado o controle, some o cartão, ficam o preto, o pad e a pílula "Sair do controle". Alternativa descartada (versão anterior): pílula "controle ativo" sobre a imagem sempre que o jogo estava no PC, que mostrava o jogo parado e mandava INPUT sem o usuário pedir.
- **A pílula e o chip "Enviar ao PC" ficam no canto livre.** `Placement.pick` escolhe o primeiro canto da imagem que não toca controle nem o botão de menu (com folga; o botão é posto primeiro, e a pílula e o chip desviam dele), e, se todos colidem, o de menor área sobreposta; reavalia a cada poll de 300 ms. Na ordem z o pad fica ABAIXO dos chips: o `PadView` consome todo toque, então um chip por baixo nunca receberia clique.
- **Configurações em `SharedPreferences("app")`** (`prefer_newest`, `control_mode`), não no motor: o motor só guarda o valor atual (`phone_set_prefer_newest`/`phone_set_control` na abertura do app). Mudar a opção no celular anuncia ao PC (CONTROL SETTINGS) e, se diferirem, aparece "As configurações dos dois aparelhos são diferentes" (registro `D`) para escolher o padrão dos dois. A pergunta "Continuar no: Celular | PC" (registro `Q`) vem antes da de configurações no mesmo `ChoicePanel`.
