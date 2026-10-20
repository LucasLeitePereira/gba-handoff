---
titulo: Decisões do protocolo gbahp e do transporte PC-celular
tipo: decisao
gerado_por: claude-sonnet-5-5
commit: sem-commit
arquivos: proto/gbahp.h, proto/gbahp.c, proto/gbahp_net.c, desktop/link.cpp, android/app/src/main/cpp/link_jni.c, android/app/src/main/java/io/gbahandoff/LinkClient.kt
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T02:00:08Z
parecer: Conferi o documento no código (proto/gbahp.h, proto/gbahp.c, proto/gbahp_net.c, desktop/link.cpp, desktop/adb.cpp, desktop/main.cpp, link_jni.c, LinkClient.kt, MainActivity.kt). Quase tudo bate: header de 16 bytes, limite de 64 MiB, parser que cresce o buffer conforme os dados chegam e fica envenenado após erro, gbahp_hello_compat comparando protocolo e versão do core, ping a cada 5 s e queda após 15 s de silêncio, a conexão nova derrubando a antiga, retry de 2 s com backoff x1,5 até 10 s, espera de 10 s quando recusado, consulta da UI a cada 1 s, adb/adb.exe com o PATH como alternativa, `adb devices` a cada 2 s só enquanto desconectado e reverse refeito a cada 10 s.; Correção 1: a porta não é fixa no PC. 27183 é o padrão (GBAHP_DEFAULT_PORT), e o desktop aceita `--port`. Só o celular usa a porta fixa. Se alguém mudar a porta no PC, o celular para de conectar.; Correção 2: o app mostra 'versão do emulador incompatível' para qualquer recusa (ST_REFUSED), inclusive quando o que difere é a versão do protocolo. Isso pode confundir quem estiver diagnosticando.; Não repete os documentos aprovados: decisoes-arquitetura-plataforma só diz que o transporte é 'adb reverse + TCP', sem detalhes do protocolo. Explica o porquê das decisões e não tem segredos nem dados pessoais.
---

# Decisões do protocolo gbahp e do transporte

Contexto: PC (servidor) e celular (cliente) conversam por TCP sobre `adb reverse` (ver decisoes-arquitetura-plataforma).

- **Framing próprio de 16 bytes** (magic `GBAH`, versão, tipo, flags, tamanho u32, CRC32 do payload), tudo little-endian e
  codificado campo a campo, nunca `memcpy` de struct (padding/endianness variam entre mingw, NDK e host). Limite de
  payload 64 MiB. O parser é incremental e cresce o buffer só conforme os dados chegam, então um tamanho mentiroso não
  custa memória. Erro de magic/CRC "envenena" o parser: a conexão é derrubada, não há ressincronização.
- **HELLO leva a versão do mGBA.** O servidor recusa (HELLO_ACK com status != OK e fecha) se a versão do protocolo
  **ou** a string de versão do core forem diferentes: o state só carrega na mesma versão do core (contrato-handoff-estado).
  O app mostra "versão do emulador incompatível" para qualquer recusa (inclusive versão de protocolo diferente) e tenta
  de novo só a cada 10 s para a mensagem não sumir.
- **Só loopback, porta 27183**: o servidor escuta em 127.0.0.1 (nada exposto na rede) e o celular conecta em
  `localhost`, com o `adb reverse` levando ao PC. No celular a porta é fixa; no PC é o padrão e pode ser trocada com
  `--port`, mas aí o celular não conecta mais.
- **Cliente Android em C (`link_jni.c`), não Kotlin.** Reusa o mesmo `proto/` do desktop (codec e parser existem uma
  vez) e evita JNI por mensagem. Uma thread nativa só; a UI consulta `status()`/`peerName()` a cada 1 s (sem callbacks
  para Java, logo sem attach de JNIEnv). Reconexão: 2 s, com backoff x1,5 até 10 s.
- **Protocolo simétrico:** os dois lados enviam LIBRARY. Hoje é só informativo (o celular manda lista vazia; o PC loga).
  O celular dá PING a cada 5 s; o servidor responde PONG, e derruba a sessão após 15 s sem nenhum frame.
- **Servidor atende uma conexão por vez e a nova vence:** se chega conexão enquanto há uma ativa, a antiga (provavelmente
  morta, app reiniciado) é derrubada.
- **Miniatura do STATE_PUSH crua, XBGR8888 240x160 (`GBAHP_THUMB_BYTES`).** Decisão provisória (~150 KB sem compressão);
  mensagens ROM_PUSH/STATE_PUSH já têm codec e testes, mas o handoff em si ainda não os usa (o servidor ignora esses tipos).
- **adb empacotado ao lado do .exe** (`adb/adb.exe`; senão `adb` do PATH), para o usuário não instalar nada. O PC faz
  `adb devices` a cada 2 s **só enquanto desconectado** e refaz o `reverse` a cada 10 s enquanto não conecta (reiniciar
  o servidor adb perde o reverse). Notas de estado vão para o título da janela.
