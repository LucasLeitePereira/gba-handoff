---
titulo: Armadilhas do link PC-celular (adb, sockets, processos, ciclo de vida)
tipo: armadilha
gerado_por: claude-sonnet-5-5
commit: sem-commit
arquivos: desktop/proc.cpp, desktop/adb.cpp, proto/gbahp_net.c, desktop/link.cpp, desktop/link.h, android/app/src/main/cpp/link_jni.c, android/app/src/main/java/io/gbahandoff/LinkClient.kt
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T01:59:04Z
parecer: Conferi cada afirmação no código e nenhuma é contradita. desktop/proc.h documenta que run() não espera EOF porque o daemon do adb herda o pipe. Em proc.cpp, a espera é pelo processo com timeout (TerminateProcess no Windows, SIGKILL no POSIX) e só se drena o que já está no pipe, com PeekNamedPipe ou poll(…, 0). A lista PROC_THREAD_ATTRIBUTE_HANDLE_LIST tem só NUL e o pipe de escrita, e um comentário explica o vazamento do socket de escuta. O comando é convertido de UTF-8 para UTF-16 (MultiByteToWideChar) e segue as regras de aspas do CommandLineToArgvW.; Em proto/gbahp_net.c, no_inherit() usa SetHandleInformation(HANDLE_FLAG_INHERIT, 0) no Windows e FD_CLOEXEC no POSIX. SO_REUSEADDR só é ligado fora do Windows, e um comentário explica o roubo de porta. A falha de bind é tratada como outra instância aberta em link.cpp:68 e main.cpp:216. A porta 27183 está em main.cpp.; O namespace do desktop é `cable` (link.h, link.cpp, adb.cpp). A colisão de `namespace link` com a função ::link() do unistd.h é um erro real de C++ (redeclaração como outro tipo de entidade).; Em link_jni.c, nativeStop não bloqueia: só dá shutdown no socket, e o pthread_join fica no próximo nativeStart. Em LinkClient.kt, STOP_DELAY_MS = 1500L; onBackground agenda a parada com postDelayed e onForeground a cancela com removeCallbacks. O comentário do arquivo cita a troca entre Activities e o HELLO refeito.; Não duplica nada: os documentos aprovados parecidos tratam de arquitetura e de armadilhas de build. Nenhum cobre processos, herança de handles, sockets nem o ciclo de vida do link.; Cada item traz o sintoma, a causa e a solução escolhida, então explica o porquê em vez de resumir o código. Está conciso para o volume de armadilhas e não tem segredos nem dados pessoais.
---

# Armadilhas do link PC-celular

- **Ler a saída do `adb devices` até EOF trava para sempre.** Sintoma: a thread do adb nunca volta. Causa: o primeiro
  `adb` sobe o daemon, que herda o pipe de saída e nunca o fecha. Solução em `proc::run`: esperar o **processo** terminar
  (com timeout e kill) e só drenar o que já está no pipe (`PeekNamedPipe`/`poll`), nunca bloquear em EOF.
- **Sockets herdados pelo daemon do adb prendem a porta.** Sintoma: depois de fechar o app, `bind` na 27183 falha até
  matar o `adb`. Causa: o daemon herdou o socket de escuta. Solução: sockets sempre não herdáveis (`FD_CLOEXEC` no POSIX;
  `HANDLE_FLAG_INHERIT=0` no Windows) **e** no Windows o `CreateProcessW` usa `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` com só
  stdin/stdout, porque um handle marcado herdável por outro código vazaria mesmo assim.
- **Windows: não usar `SO_REUSEADDR` no listener** (permite outro processo roubar a porta). No POSIX ele é usado
  (evita TIME_WAIT no teste). Falha de bind = outra instância aberta.
- **`CreateProcessW` + UTF-16:** caminhos com acento (pasta do usuário) quebram com a API ANSI. `proc.cpp` converte de
  UTF-8 e aplica as regras de aspas do `CommandLineToArgvW`.
- **Namespace C++ `link` colide com `::link()` do `unistd.h`.** Por isso o namespace do código desktop é `cable`.
- **Parar o cliente Android não pode bloquear a UI.** `nativeStop` só marca parada e dá `shutdown` no socket; o `join` da
  thread é feito no próximo `nativeStart`. Em Kotlin o stop é adiado 1,5 s (`STOP_DELAY_MS`) e cancelado pelo
  `onForeground` seguinte: sem isso, trocar entre MainActivity e GameActivity (onPause/onResume) derrubava a conexão e
  refazia o HELLO a cada troca.
