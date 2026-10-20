---
titulo: Decisões do protocolo gbahp e do transporte PC-celular
tipo: decisao
gerado_por: claude-sonnet-5-5
commit: 67beb79
arquivos: proto/gbahp.h, proto/gbahp.c, proto/gbahp_net.c, desktop/link.cpp, desktop/main.cpp, desktop/handoff.cpp, android/app/src/main/cpp/link_jni.c, android/app/src/main/cpp/phone_link.c, android/app/src/main/java/io/gbahandoff/LinkClient.kt
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T14:27:33Z
parecer: Em relação à versão aprovada (50fe505), o que muda é o novo item sobre INPUT, a menção a INPUT no item de estado da implementação e o front matter `arquivos`, que agora inclui phone_link.c e desktop/main.cpp. Isso também resolve a ressalva da revisão anterior sobre phone_link.c faltar em `arquivos`.; Conferi o item de INPUT no código e ele bate. Em proto/gbahp.h:47 está GBAHP_MSG_INPUT = 8, e na linha 219 está gbahp_input {seq, keys} na ordem do KEYINPUT. Em desktop/link.cpp:500 a linha `keys_ = in.keys` grava o estado inteiro, não eventos. Em desktop/link.cpp:574 `keys_ = 0` roda quando a sessão termina. Em desktop/main.cpp:400 está `if (link_status.connected()) keys |= server.remote_keys() & GBAH_KEY_MASK`, e o comentário diz que o código só chega ali com o PC em Playing. GBAH_KEY_MASK = 0x3FF fica em core/gbah_core.h:34.; Do lado do celular, em phone_link.c:43, INPUT_RESEND_MS vale 100. Em phone_link.c:1345, as teclas presas são reenviadas a cada 100 ms. Em phone_link.c:1575, phone_cmd_input só põe o comando na fila se o jogo está no PC. Em phone_link.c:1128, send_input só envia se o jogo está no PC. Isso confirma que o celular só envia com o jogo no PC.; A ressalva sobre `keys_` está correta. Em desktop/handoff.cpp:239, send_pc_status considera o jogo vivo também em Conflict e em Returning. Então, numa pausa por conflito, o celular continua achando que o jogo está no PC, e o PC não zera keys_. Quando o PC vai para o menu (estado não vivo), phone_link.c:1128-1129 manda keys=0 uma vez. Por isso a ressalva vale de fato só para Conflict e Returning, e o exemplo do texto (pausa por conflito) é exatamente esse caso.; O item explica o porquê: estado completo em vez de press/release, para que uma perda não deixe tecla presa, junto com o reenvio periódico. Também registra uma armadilha com o sintoma (o último INPUT vale ao voltar a Playing). É conhecimento novo, que não está em contrato-handoff-estado (lá só aparece a ordem de bits) nem nos outros documentos.; Os demais itens são iguais aos da versão aprovada, que já tinham sido conferidos. O texto é conciso para um documento de decisões e não tem segredos nem dados pessoais.
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
- **INPUT (celular -> PC) é o estado inteiro das teclas, não eventos**, na ordem de bits do KEYINPUT, com `seq`. Estado completo (e não press/release) porque uma perda não deixa tecla presa para sempre: o celular reenvia a cada 100 ms enquanto há tecla. O PC faz `keys |= remote_keys() & GBAH_KEY_MASK` (soma ao teclado/gamepad) só com sessão conectada e `keys_` é zerado quando a sessão cai. O celular só envia com o jogo no PC. Ressalva: `keys_` não é zerado quando o PC sai de Playing sem a sessão cair (ex.: pausa por conflito); ao voltar a Playing vale o último INPUT até chegar outro.
- **ROM em pedaços de 256 KB** (`GBAHP_ROM_CHUNK`; completa quando offset + tamanho do pedaço = total), para que
  mensagens pequenas (PONG, ACK, CONTROL) passem entre os pedaços. O PC ignora ROM_PUSH com tamanho total 0 ou acima
  de 32 MiB.
- **Estado da implementação (commit 67beb79):** todas as mensagens estão implementadas ponta a ponta nos dois lados
  (o celular cobre LIBRARY, STATE_PUSH/ACK, ROM, CONTROL, THUMB e INPUT).
- **Servidor atende uma conexão por vez e a nova vence:** se chega conexão enquanto há uma ativa, a antiga (provavelmente
  morta, app reiniciado) é derrubada.
- **Miniatura crua, XBGR8888 240x160 (`GBAHP_THUMB_BYTES`)**, no STATE_PUSH e na THUMB. Decisão provisória (~150 KB sem
  compressão), aceita porque só trafega junto de um state e nos conflitos.
- **adb empacotado ao lado do .exe** (`adb/adb.exe`; senão `adb` do PATH), para o usuário não instalar nada. O PC faz
  `adb devices` a cada 2 s **só enquanto desconectado** e refaz o `reverse` a cada 10 s enquanto não conecta (reiniciar
  o servidor adb perde o reverse). Notas de estado vão para o título da janela.
