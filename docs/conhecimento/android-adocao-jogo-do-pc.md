---
titulo: Celular adota o jogo que o PC já joga ao conectar (on_pc_has, registro O, tela sem ROM)
tipo: armadilha
gerado_por: claude-sonnet-5-5
commit: 93291cf
arquivos: android/app/src/main/cpp/phone_link.c, android/app/src/main/cpp/phone_link.h, android/app/src/main/cpp/link_jni.c, android/app/src/main/java/io/gbahandoff/GameActivity.kt, android/app/src/main/java/io/gbahandoff/MainActivity.kt, tests/phone_link_test.c
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T21:07:09Z
parecer: O conteúdo é novo: a adoção do jogo que o PC já roda ao conectar, o registro O como pedido de 'mostrar' com ack e a tela sem ROM não aparecem em android-motor-phone-link nem em handoff-fluxos-pc. O documento também explica o porquê: o sintoma de dois emulando, o motivo de adotar só quando o status muda (senão o jogo devolvido pelo RETURN seria readotado) e o motivo do ack (o aviso se perdia entre telas).; Conferi no código: adopt_pc_game só age sem handoff pendente; LIB_WAIT_STATUS_MS existe; o `started` de handle_status se comporta como descrito; request_show não roda com jogo carregado; open_adopt é limpo em leave_on_pc e no fim da sessão; o ack vem da MainActivity via ackOpen; GameActivity com noRom chama setView e input_allowed usa view_sha quando não há core; a tela se recria se a ROM chegar e fecha se o jogo sair do PC; o cenário I do phone_link_test confere com o descrito.; Correção 1: em process_pc_library, o caso NOTHING só adota se G NÃO estiver aberto (`pc_this && !is_cur`). Com G aberto e mesma versão, o código cai em start_handoff. Só RECEIVE adota com G aberto (é o modo 1 do teste). O texto dava a entender que NOTHING também adotava com G aberto.; Correção 2: em handle_status, a adoção em runtime também exige que nenhum jogo esteja carregado (`!loaded_sha`), e não só a mudança de status. Acrescentei isso ao texto.; Encurtei o texto em alguns pontos. Não há segredos nem dados pessoais.
---

# Celular adota o jogo que o PC já joga ao conectar

**Bug.** Com o PC jogando G e o celular conectando, `on_pc_has` só era ligado por handoff/PLACE_PC. O celular não sabia que G estava "no PC", então ao abrir G emulava a cópia local: dois emulando e conflito na volta. Regra do dono único: ver android-motor-phone-link.

**Correção: `adopt_pc_game` (liga `on_pc_has`, que retém o jogo: não emula, não grava; só sem handoff pendente).** Dois pontos, de propósito diferentes:
- `process_pc_library` (na conexão), quando o PC joga G e nenhum OUTRO jogo está aberto: adota em NOTHING (mesma versão) só se G não estiver aberto, porque com G aberto a mesma versão segue para `start_handoff`; e adota em RECEIVE (o PC está à frente; a cópia dele só é guardada) mesmo com G aberto e ocioso. Antes de processar a LIBRARY o motor espera o PC_STATUS (ou `LIB_WAIT_STATUS_MS`), então já sabe o que o PC joga.
- `handle_status` (em runtime, depois da LIBRARY): adota só numa MUDANÇA para "joga G" (`started`) e sem jogo carregado. Motivo: na devolução (RETURN) o PC manda o push de handoff e depois PC_STATUS ainda com G. Se adotasse a cada status G, readotaria o jogo recém-devolvido e o prenderia no PC.
- **Pendência conhecida:** SEND (celular à frente do PC que joga G) não adota; cai em `push_game`/handoff como antes.

**Registro `O` agora também significa "mostre o jogo que o PC joga"** (`open_adopt`, `request_show`). Só sai se nenhum jogo estiver carregado (nunca interrompe tela de jogo) e some com a sessão ou com `leave_on_pc`. O ack vem da `MainActivity` (`ackOpen`), não da leitura por polling: o `O` fica no texto até o ack. Sem isso, um aviso lido por uma tela que já tinha fechado se perdia entre telas. Só a `MainActivity` age sobre ele.

**Sem a ROM no celular** (`GameActivity` recebe sha1 sem URI e `RomLibrary` não o conhece): não cria core, mostra só "No PC" e o controle (`noRom`). `phone_set_view(sha1)` diz ao motor qual jogo a tela mostra, porque `input_allowed` exigia um core carregado para liberar o modo controle. Se a ROM chegar do PC, a tela se recria; se o PC parar de jogar, ela fecha.

**Verificar:** `phone_link_test` cenário I. Modo 0: menu aberto, `O` uma vez, abrir G fica congelado. Modo 1: G aberto e ocioso, PC à frente, sem conflito. Nos dois, depois vem um RETURN sem conflito e só o celular avança. Sintoma de regressão: frames do celular avançando enquanto o PC joga G, ou `has_rec("O ")` voltando após o ack.
