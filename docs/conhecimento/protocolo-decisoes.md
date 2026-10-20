---
titulo: Decisões do protocolo gbahp e do transporte PC-celular
tipo: decisao
gerado_por: claude-sonnet-5-5
commit: 74f70ce
arquivos: proto/gbahp.h, proto/gbahp.c, proto/gbahp_net.c, desktop/link.cpp, desktop/main.cpp, desktop/handoff.cpp, android/app/src/main/cpp/link_jni.c, android/app/src/main/cpp/phone_link.c, android/app/src/main/java/io/gbahandoff/LinkClient.kt
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T17:53:25Z
parecer: Conferi no código atual (74f70ce + WT) cada mudança em relação à versão aprovada. Em proto/gbahp.h:31, GBAHP_VERSION é 3. As mensagens de recusa estão em desktop/link.cpp:462-466 (PC: 'versao do app do celular (vN) diferente da do PC (vM): atualize o app do celular e o do PC') e em LinkUi.kt:63-64 (celular, a partir do registro 'X <status> <proto>' em phone_link.c:1838).; Os itens de CONTROL batem com o código. O enum em proto/gbahp.h:208-215 inclui PLACE_PC/PLACE_PHONE e SETTINGS/SETTINGS_SET com GBAHP_SET_PREFER_NEWEST. A armadilha de GBAHP_CTL_LAST está certa: proto/gbahp.c:536 e :547 recusam cmd > GBAHP_CTL_LAST tanto na codificação quanto na decodificação. RETURN é aceito em Playing ou AwaitPlace (handoff.cpp:735). FETCH leva a AwaitPlace ('our version was taken', handoff.cpp:698), então o PC não volta a jogar. O comentário do header em gbahp.h:209 ('you keep playing') está desatualizado, mas o documento está certo.; Os itens de INPUT batem com o código. O modo controle é opcional e desligado por padrão: control_mode tem default false em LinkClient.kt:176, e phone_cmd_input só aceita com control_on (phone_link.c:1762). Quando input_allowed falha, send_input manda keys=0 uma única vez (phone_link.c:1215-1217). No PC, clear_keys roda ao sair de Playing (set_mode, handoff.cpp:97), ao entrar em Playing (handoff.cpp:286) e quando a sessão cai (link.cpp:600). Isso resolve a ressalva da versão anterior, que deixava de valer e foi removida corretamente.; O documento continua explicando o porquê (subir a versão por mudança de comportamento, estado completo em vez de eventos, mensagem de recusa que diz o que fazer). Substitui a versão aprovada sem duplicar outros documentos, segue conciso para um documento de decisões e não tem segredos nem dados pessoais.
---

# Decisões do protocolo gbahp e do transporte

Contexto: PC (servidor) e celular (cliente) conversam por TCP sobre `adb reverse` (ver decisoes-arquitetura-plataforma).

- **Framing próprio de 16 bytes** (magic `GBAH`, versão, tipo, flags, tamanho u32, CRC32 do payload), tudo little-endian e
  codificado campo a campo, nunca `memcpy` de struct (padding/endianness variam entre mingw, NDK e host). Limite de
  payload 64 MiB. O parser é incremental e cresce o buffer só conforme os dados chegam, então um tamanho mentiroso não
  custa memória. Erro de magic/CRC "envenena" o parser: a conexão é derrubada, não há ressincronização.
- **HELLO leva a versão do protocolo (`GBAHP_VERSION`, hoje 3) e a do mGBA.** O servidor recusa (HELLO_ACK com
  PROTO_MISMATCH ou CORE_MISMATCH e fecha) se qualquer uma for diferente: o state só carrega na mesma versão do core
  (contrato-handoff-estado). A versão sobe quando muda o comportamento, não só o formato: a v3 trouxe PLACE_*/SETTINGS e o
  dono único, e um celular v2 (que retomava sozinho com o cabo puxado) é recusado de propósito. A recusa diz o que fazer:
  no PC aparece "versão do app do celular (vN) diferente da do PC (vM): atualize o app do celular e o do PC" (título e
  menu); no celular, o registro `X <status> <proto_do_pc>` vira "atualize o app" (protocolo) ou "mGBA diferente" (core).
  O app tenta de novo só a cada 10 s para a mensagem não sumir.
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
  por HANDOFF; o PC aceita também em AwaitPlace), FETCH (celular -> PC: mande sua versão como RESOLVED; o PC NÃO volta a
  jogar, vai a AwaitPlace), PC_STATUS (PC -> celular: jogo em execução, ou ocioso), ROM_UNAVAILABLE, PLACE_PC /
  PLACE_PHONE (celular -> PC: onde continuar depois de um conflito resolvido) e SETTINGS / SETTINGS_SET (bits
  `GBAHP_SET_*`; SETTINGS é anúncio logo após o HELLO, nos dois sentidos; SETTINGS_SET é a escolha do usuário como padrão
  dos dois, o PC adota e grava). Armadilha: o codec recusa `cmd > GBAHP_CTL_LAST`; ao criar um comando novo, atualize
  `GBAHP_CTL_LAST`, senão ele é descartado na codificação e na decodificação.
- **INPUT (celular -> PC) é o estado inteiro das teclas, não eventos**, na ordem de bits do KEYINPUT, com `seq`. Estado completo (e não press/release) porque uma perda não deixa tecla presa para sempre: o celular reenvia a cada 100 ms enquanto há tecla. **Só existe no modo controle (opcional, desligado por padrão) e só se o jogo aberto é o que o PC está jogando**; fora disso o celular manda `keys=0` uma vez e cala. O PC faz `keys |= remote_keys() & GBAH_KEY_MASK` e zera `keys_` quando a sessão cai, quando sai de Playing (`set_mode`) e ao entrar em Playing, para uma tecla "segurada" não atravessar uma pausa ou troca de dono.
- **ROM em pedaços de 256 KB** (`GBAHP_ROM_CHUNK`; completa quando offset + tamanho do pedaço = total), para que
  mensagens pequenas (PONG, ACK, CONTROL) passem entre os pedaços. O PC ignora ROM_PUSH com tamanho total 0 ou acima
  de 32 MiB.
- **Estado da implementação (commit 74f70ce):** todas as mensagens estão implementadas ponta a ponta nos dois lados.
- **Servidor atende uma conexão por vez e a nova vence:** se chega conexão enquanto há uma ativa, a antiga (provavelmente
  morta, app reiniciado) é derrubada.
- **Miniatura crua, XBGR8888 240x160 (`GBAHP_THUMB_BYTES`)**, no STATE_PUSH e na THUMB. Decisão provisória (~150 KB sem
  compressão), aceita porque só trafega junto de um state e nos conflitos.
- **adb empacotado ao lado do .exe** (`adb/adb.exe`; senão `adb` do PATH), para o usuário não instalar nada. O PC faz
  `adb devices` a cada 2 s **só enquanto desconectado** e refaz o `reverse` a cada 10 s enquanto não conecta (reiniciar
  o servidor adb perde o reverse). Notas de estado vão para o título da janela.
