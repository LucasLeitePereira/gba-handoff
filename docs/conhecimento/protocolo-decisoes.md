---
titulo: Decisões do protocolo gbahp e do transporte PC-celular
tipo: decisao
gerado_por: claude-sonnet-5-5
commit: 50fe505
arquivos: proto/gbahp.h, proto/gbahp.c, proto/gbahp_net.c, desktop/link.cpp, desktop/handoff.cpp, android/app/src/main/cpp/link_jni.c, android/app/src/main/java/io/gbahandoff/LinkClient.kt
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T13:44:51Z
parecer: Conferi no código as afirmações que mudaram desde a versão f84dedf. Em android/app/src/main/cpp/link_jni.c:1-8, o comentário diz que toda a lógica está em phone_link.c e que este arquivo só faz a cola JNI. Diz também que a UI consulta phone_ui_text() (nativeUiText, linha 139). Isso bate com 'link_jni.c só cola' e com 'a UI consulta o estado por texto'.; Em phone_link.c:1408 e :1436 há duas threads (save_main e link_main), como diz o texto: 'uma de link e uma de autosave'. A reconexão bate com o código: RETRY_MIN_MS 2000, RETRY_MAX_MS 10000 e delay*3/2 nas linhas 34-35 e 1371-1372. Depois de PL_ST_REFUSED o app espera RETRY_MAX_MS (linha 1382), ou seja, os 10 s. PING_EVERY_MS 5000 e IDLE_TIMEOUT_MS 15000 estão nas linhas 36-37.; O item 'todas as mensagens implementadas ponta a ponta nos dois lados' se confirma. O celular codifica LIBRARY com entradas reais (linha 1011), STATE_PUSH (536), STATE_ACK (457), ROM_REQUEST (678), ROM_PUSH em pedaços (854), CONTROL RETURN/FETCH/ROM_UNAVAILABLE (806, 1116, 1120) e PING (1324). O laço de recepção (1237-1293) trata LIBRARY, CONTROL (PC_STATUS e ROM_UNAVAILABLE), STATE_PUSH, STATE_ACK, ROM_REQUEST, ROM_PUSH e THUMB. Só o PC envia THUMB (handoff.cpp:212) e o celular a recebe; isso cobre o fluxo de conflito, então o texto continua correto.; A mensagem 'versão do emulador incompatível' para qualquer REFUSED continua em MainActivity.kt:85. Os demais itens são iguais aos da versão aprovada, que já tinham sido conferidos.; O documento substitui conhecimento/protocolo-decisoes.md e só atualiza o que mudou na Fase 4b: o cliente agora fica em phone_link.c, com duas threads, e a implementação está completa no celular. Não repete decisoes-arquitetura-plataforma nem contrato-handoff-estado, só remete a eles. Explica os porquês: framing campo a campo, só loopback, cliente em C para reusar proto/, ROM em pedaços para não travar PONG/ACK e a miniatura crua provisória. Não tem segredos nem dados pessoais.; Ressalva que não bloqueia a aprovação: o front matter `arquivos` deveria incluir android/app/src/main/cpp/phone_link.c, que agora é onde fica a lógica do cliente.
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
- **Cliente Android em C, não Kotlin** (`phone_link.c`; `link_jni.c` só cola). Reusa o mesmo `proto/` do desktop
  e evita JNI por mensagem; a UI consulta o estado por texto (android-motor-phone-link). Uma thread de link
  (reconexão: 2 s, backoff x1,5 até 10 s) e uma de autosave. O celular dá PING a cada 5 s; o servidor responde PONG e
  derruba a sessão após 15 s sem nenhum frame.
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
- **Estado da implementação (commit 50fe505):** todas as mensagens estão implementadas ponta a ponta nos dois lados
  (o celular cobre LIBRARY, STATE_PUSH/ACK, ROM, CONTROL e THUMB).
- **Servidor atende uma conexão por vez e a nova vence:** se chega conexão enquanto há uma ativa, a antiga (provavelmente
  morta, app reiniciado) é derrubada.
- **Miniatura crua, XBGR8888 240x160 (`GBAHP_THUMB_BYTES`)**, no STATE_PUSH e na THUMB. Decisão provisória (~150 KB sem
  compressão), aceita porque só trafega junto de um state e nos conflitos.
- **adb empacotado ao lado do .exe** (`adb/adb.exe`; senão `adb` do PATH), para o usuário não instalar nada. O PC faz
  `adb devices` a cada 2 s **só enquanto desconectado** e refaz o `reverse` a cada 10 s enquanto não conecta (reiniciar
  o servidor adb perde o reverse). Notas de estado vão para o título da janela.
