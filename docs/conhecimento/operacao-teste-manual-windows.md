---
titulo: Teste manual do handoff no Windows: roteiro e falsas falhas
tipo: operacao
gerado_por: claude-sonnet-5-5
commit: 8b8c4de
arquivos: desktop/main.cpp, tools/fetch-adb-windows.sh, tools/package-windows.sh, tests/fake_adb.sh, tests/CMakeLists.txt
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T19:21:58Z
parecer: Conferi no código e as afirmações estão corretas. desktop/main.cpp:843 abre a tela Esc em jogo, e main.cpp:838 fecha o programa quando Esc é tecado no menu principal. main.cpp:873-880 pausa o áudio sempre que uma tela está aberta sobre o jogo. main.cpp:816 trata o F11. handoff.cpp:379 recusa a importação em Playing/Conflict/Returning com 'Volte ao menu para importar'. handoff.cpp:385-396 adota o .sav só quando a ROM é nova (added) e não sobrescreve um existente. handoff.cpp:370 mostra 'O celular esta jogando: devolva por la'. main.cpp:439 mostra PORTA OCUPADA e main.cpp:451/455 mostra 'GBAHandoff.apk nao encontrado ao lado do .exe'. tools/package-windows.sh copia o APK de debug do Gradle e só avisa quando ele falta. tests/CMakeLists.txt:38 usa file(COPY), que roda na configuração do CMake. tests/fake_adb.sh:11-13 usa FAKE_PKG_VERSION (vazia = pacote ausente) e FAKE_INSTALL.; O que é novo e vale entrar na base: Esc em jogo agora abre um menu e pausa, em vez de voltar ao menu. Isso corrige a versão aprovada, que dizia que Esc 'volta ao menu' e ficou desatualizada. Também são novos o sintoma do APK ausente e a armadilha da cópia do fake_adb.sh feita só na configuração, que nenhum documento aprovado cobre (link-operacao só cita o fake).; Ajuste 1: tirei a lista completa de itens do menu Esc. É descrição do código e está na tela; o pendente pc-configuracoes-teclas-volume já trata do menu e do remapeamento.; Ajuste 2: tirei 'Documentos e hábitos antigos diziam o contrário', que é vago. No lugar, deixei claro o que muda para quem testa: sair do jogo agora leva dois passos.; Ajuste 3: no item do APK, deixei o motivo explícito: o package-windows.sh nunca roda o Gradle. Voltei com 'Reimportar com outro .sav não muda nada', da versão aprovada, que é o sintoma concreto. Os detalhes da instalação ficam como referência a pc-instalacao-app-adb.; Sem segredos nem dados pessoais.
---

# Teste manual do handoff no Windows

O caminho real no Windows (adb de verdade, janela SDL, drag-and-drop) não tem teste automático (ver link-operacao). Para validar, monte o pacote com `tools/package-windows.sh` (exe + `adb/` + `GBAHandoff.apk`; ver pc-instalacao-app-adb) e use um celular com depuração USB. O próprio exe instala o app.

Roteiro: arraste um `.gba` para a janela do menu, abra o jogo e use **F2** para entregar ao celular, **F5**/**F8** para salvar/recarregar o ponto salvo, **F11** para tela cheia e **Esc** para o menu do jogo.

Comportamentos esperados que parecem bug:
- **Esc em jogo não volta ao menu**: abre o menu do jogo e pausa o jogo e o áudio. Para sair, escolha "Voltar ao menu" nele. Só no menu principal o Esc fecha o programa.
- **Arrastar a ROM durante o jogo** é recusado ("Volte ao menu para importar"). Importe pelo menu.
- **`.sav` ao lado da ROM** (`x.gba.sav` ou `x.sav`) só é adotado na *primeira* importação daquela ROM e nunca sobrescreve um save que já existe. Reimportar com outro `.sav` não muda nada.
- **Depois do F2** ("Jogando no celular"), o Enter só retoma no PC com o **cabo desconectado**. Com o cabo, o PC responde "O celular esta jogando: devolva por la".
- **Segunda instância** mostra "PORTA OCUPADA: outra instancia ja esta aberta". A porta TCP é exclusiva; não é falha de rede nem do adb.
- **"GBAHandoff.apk nao encontrado ao lado do .exe"** no lugar do Instalar: o zip foi gerado sem o APK de debug. O `package-windows.sh` nunca roda o Gradle; ele só avisa e gera o zip sem o APK. Compile o app Android antes.

Armadilha do teste automático: `tests/fake_adb.sh` é copiado para `build-linux/tests` por `file(COPY)`, que só roda na configuração do CMake. Se você editar o script e rodar só `ctest`, o teste usa a cópia velha; rode `cmake` de novo antes. Variáveis do fake: `FAKE_PKG_VERSION` (vazia = app não instalado) e `FAKE_INSTALL` (saída do `install`; padrão `Success`).
