---
titulo: Decisões do protocolo gbahp e do transporte PC-celular
tipo: decisao
gerado_por: claude-sonnet-5-5
commit: f84dedf
arquivos: proto/gbahp.h, proto/gbahp.c, proto/gbahp_net.c, desktop/link.cpp, desktop/handoff.cpp, android/app/src/main/cpp/link_jni.c, android/app/src/main/java/io/gbahandoff/LinkClient.kt
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T12:37:26Z
parecer: Conferi as afirmações novas no código e elas batem com o lado PC. Em proto/gbahp.h: GBAHP_VERSION 2, GBAHP_LIB_ENTRY_BYTES 89 com sha1, size (0 = sem ROM), has_data, versão, base, timestamp e frame, as flags MIRROR/RESOLVED/HANDOFF, os status OK/CONFLICT/NO_ROM/ERROR/STALE, os comandos CONTROL RETURN/FETCH/PC_STATUS/ROM_UNAVAILABLE, a mensagem THUMB e GBAHP_ROM_CHUNK de 256 KB. Em desktop/link.cpp:361, kMaxRomSize é 32 MiB e o tamanho total 0 também é ignorado. Em desktop/handoff.cpp, held_ segura o push sem ROM e responde NO_ROM quando chega ROM_UNAVAILABLE. Em proto/gbahp.c, PROTO_MISMATCH e CORE_MISMATCH estão no gbahp_hello_compat. MainActivity.kt ainda mostra 'versão do emulador incompatível' para qualquer ST_REFUSED.; Correção: 'os dois lados enviam LIBRARY, e agora ela é a base da decisão de sync' vale só para o PC. Em link_jni.c:117 o celular ainda manda LIBRARY vazia ('nothing to offer yet') e ignora as mensagens que recebe além de PING/PONG (linha 136). Ajustei o texto para dizer que, no commit f84dedf, o celular ainda não implementa sync, handoff nem ROM_PUSH. Sem isso, quem estiver depurando vai procurar a lógica no app.; Tirei uma frase repetida: a recusa por versão de protocolo diferente já aparece no item do HELLO. Agora o item do HELLO também cita os códigos PROTO_MISMATCH/CORE_MISMATCH.; O documento substitui a versão aprovada e atualiza o que mudou: antes a LIBRARY era só informativa e o servidor ignorava ROM_PUSH/STATE_PUSH. Não repete decisoes-arquitetura-plataforma nem contrato-handoff-estado e remete a sync-regras-versao para as regras de versão. Não tem segredos nem dados pessoais.
---

# Decisões do protocolo gbahp e do transporte

Contexto: PC (servidor) e celular (cliente) conversam por TCP sobre `adb reverse` (ver decisoes-arquitetura-plataforma).

- **Framing próprio de 16 bytes** (magic `GBAH`, versão, tipo, flags, tamanho u32, CRC32 do payload), tudo little-endian e
  codificado campo a campo, nunca `memcpy` de struct (padding/endianness variam entre mingw, NDK e host). Limite de
  payload 64 MiB. O parser é incremental e cresce o buffer só conforme os dados chegam, então um tamanho mentiroso não
  custa memória. Erro de magic/CRC "envenena" o parser: a conexão é derrubada, não há ressincronização.
- **HELLO leva a versão do protocolo (`GBAHP_VERSION`, hoje 2) e a do mGBA.** O servidor recusa (HELLO_ACK com
  PROTO_MISMATCH ou CORE_MISMATCH e fecha) se qualquer uma for diferente: o state só carrega na mesma versão do core
  (contrato-handoff-estado). O app mostra "versão do emulador incompatível" para qualquer recusa (inclusive versão de
  protocolo diferente) e tenta de novo só a cada 10 s para a mensagem não sumir.
- **Só loopback, porta 27183**: o servidor escuta em 127.0.0.1 (nada exposto na rede) e o celular conecta em
  `localhost`, com o `adb reverse` levando ao PC. No celular a porta é fixa; no PC é o padrão e pode ser trocada com
  `--port`, mas aí o celular não conecta mais.
- **Cliente Android em C (`link_jni.c`), não Kotlin.** Reusa o mesmo `proto/` do desktop (codec e parser existem uma
  vez) e evita JNI por mensagem. Uma thread nativa só; a UI consulta `status()`/`peerName()` a cada 1 s (sem callbacks
  para Java, logo sem attach de JNIEnv). Reconexão: 2 s, com backoff x1,5 até 10 s. O celular dá PING a cada 5 s; o
  servidor responde PONG e derruba a sessão após 15 s sem nenhum frame.
- **LIBRARY simétrica é a base da decisão de sync.** Cada entrada (89 bytes) leva SHA-1, tamanho da ROM (0 = sem o
  arquivo), `has_data` e, havendo progresso, versão (id do aparelho + counter), base, timestamp e frame (regras em
  sync-regras-versao). Não leva imagens: para conflito existe a mensagem THUMB própria.
- **STATE_PUSH com flags, ACK com status.** MIRROR = cópia de segurança de um jogo que o remetente continua jogando
  (guarda, nunca assume); HANDOFF = o remetente parou, o receptor assume o state; RESOLVED = o usuário escolheu esta
  versão num conflito (sobrescreve; a antiga vira backup). O push carrega versão e base para o receptor decidir sozinho.
  STATE_ACK devolve OK, CONFLICT, NO_ROM, ERROR ou STALE (receptor já tem algo mais novo; nada guardado). Sem a ROM, o
  receptor segura o push, pede ROM_REQUEST e só aplica e dá o ACK quando a ROM chega verificada; quem não a tem responde
  CONTROL(ROM_UNAVAILABLE), e o push segurado recebe NO_ROM.
- **CONTROL para comandos pequenos que não são transferência de estado:** RETURN (celular -> PC: pare e devolva o jogo
  por HANDOFF), FETCH (celular -> PC: mande sua versão como RESOLVED, você segue jogando), PC_STATUS (PC -> celular:
  jogo em execução, ou ocioso) e ROM_UNAVAILABLE.
- **ROM em pedaços de 256 KB** (`GBAHP_ROM_CHUNK`; completa quando offset + tamanho do pedaço = total), para que
  mensagens pequenas (PONG, ACK, CONTROL) passem entre os pedaços. O PC ignora ROM_PUSH com tamanho total 0 ou acima
  de 32 MiB.
- **Estado da implementação (commit f84dedf):** LIBRARY, STATE_PUSH/ACK, ROM_PUSH, CONTROL e THUMB estão implementados
  ponta a ponta só no desktop. O cliente Android ainda manda LIBRARY vazia e ignora tudo além de PING/PONG.
- **Servidor atende uma conexão por vez e a nova vence:** se chega conexão enquanto há uma ativa, a antiga (provavelmente
  morta, app reiniciado) é derrubada.
- **Miniatura crua, XBGR8888 240x160 (`GBAHP_THUMB_BYTES`)**, no STATE_PUSH e na THUMB. Decisão provisória (~150 KB sem
  compressão), aceita porque só trafega junto de um state e nos conflitos.
- **adb empacotado ao lado do .exe** (`adb/adb.exe`; senão `adb` do PATH), para o usuário não instalar nada. O PC faz
  `adb devices` a cada 2 s **só enquanto desconectado** e refaz o `reverse` a cada 10 s enquanto não conecta (reiniciar
  o servidor adb perde o reverse). Notas de estado vão para o título da janela.
