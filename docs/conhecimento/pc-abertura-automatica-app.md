---
titulo: PC abre o app do celular sozinho (AdbWatcher::maybe_auto_open) e por que só uma vez
tipo: decisao
gerado_por: claude-sonnet-5-5
commit: 93291cf
arquivos: desktop/adb.cpp, desktop/link.h, desktop/main.cpp, android/app/src/main/java/io/gbahandoff/MainActivity.kt, android/app/src/main/AndroidManifest.xml, tests/autoopen_test.c
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T21:08:12Z
parecer: Conferi no código e as afirmações batem. `kAutoOpenWaitMs = 3000`. `maybe_auto_open` só age com `PhoneState::Ready`, marca `done.insert(serial)` antes de checar `pc_playing_` e `st_.connected()` e roda `am start -n io.gbahandoff/.MainActivity --ez autoOpen true`. `set_pc_playing(hub.mode() == Hub::Mode::Playing)` é chamado a cada frame em main.cpp:883. `auto_done` só guarda os seriais que ainda aparecem em `adb devices`. Com sessão ativa, os seriais de `reversed` e `ready_since` entram em `auto_done`.; No app também confere: o manifesto tem `launchMode=singleTop`. `MainActivity.onCreate` chama `finish()` quando há o extra autoOpen e `GameActivity.alive > 0`. `onPause` retorna se `notice` não foi inicializado e `onResume` retorna se `isFinishing`. `alive` é incrementado em `onCreate` e decrementado em `onDestroy` da GameActivity.; O teste confere: `autoopen_test` tem os cenários A, B, C e D com as contagens descritas (A == 1; B, C e D == 0; B e D exigem `dumpsys package` >= 1) e espera com `sleep(14)`.; O conhecimento é novo. Os documentos aprovados (protocolo-decisoes, link-armadilhas, android-decisoes-app) não falam da abertura automática. O android-adocao-jogo-do-pc cobre o que acontece depois que o app abre, e o documento só remete a ele.; O documento explica o porquê e não só o que o código faz. Abre uma vez por conexão física e nunca em laço. A chance é gasta mesmo quando o PC decide não abrir, e o texto dá as consequências: se o PC só começa a jogar depois, o app não abre, e fechar o app não o relança. Também explica por que a MainActivity se fecha quando há uma GameActivity viva: para não cobrir o jogo.; É conciso e não tem segredos nem dados pessoais.
---

# PC abre o app do celular sozinho

**O que faz.** Aparelho `Ready` (app instalado e atual) por 3 s (`kAutoOpenWaitMs`) sem sessão, com o Hub em Playing (`set_pc_playing`, chamado a cada frame no main), recebe `adb shell am start -n io.gbahandoff/.MainActivity --ez autoOpen true`. O app então conecta e mostra o jogo do PC (ver android-adocao-jogo-do-pc).

**Uma vez por conexão física, nunca em laço.** `auto_done` guarda o serial e só o esquece quando o aparelho some do `adb devices`; religar o cabo é nova conexão. A decisão é marcada ANTES de checar as condições: mesmo "não abrir" (PC fora de Playing aos 3 s, ou sessão já ativa) consome a chance. Consequência: se o PC só começar a jogar depois, não há abertura automática naquela conexão; e fechar o app não o relança. Se uma sessão já existe (o app abriu sozinho), o serial também entra em `auto_done`.

**No app.** `MainActivity` é `singleTop`. Com o extra `autoOpen` e uma `GameActivity` viva (`GameActivity.alive`, contador de telas criadas e não destruídas), a `MainActivity` se fecha em `onCreate` (`onPause`/`onResume` têm guardas para esse caso) para não cobrir o jogo.

**Verificar:** `ctest -R autoopen_test` roda quatro desktops com `fake_adb` e procura a linha do `am start` no log: A (PC joga, sem sessão) exatamente 1; B (PC no menu), C (celular conecta antes dos 3 s) e D (app não instalado) 0. B e D também exigem `dumpsys package` no log, para provar que viram o aparelho e escolheram não abrir. O teste dura ~14 s por causa da espera de 3 s e dos polls de 2 s.
