---
titulo: Instalação e atualização do app do celular pelo exe (VERSION, chave única, consulta de versão)
tipo: decisao
gerado_por: claude-sonnet-5-5
commit: 2199055
arquivos: desktop/adb.cpp, desktop/adbstate.cpp, desktop/adbstate.h, CMakeLists.txt, VERSION, android/app/build.gradle.kts, tools/package-windows.sh
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T19:23:13Z
parecer: Conferido no código: VERSION, version.h, Gradle, dumpsys, NoApp/Outdated/Ready, `install -r`, mesma chave em debug e release, `GBAH_KEYSTORE*` e package-windows.sh batem com o documento.; Duas afirmações estão erradas. (1) 'Toda mensagem de falha manda NÃO desinstalar' não vale: a falha INSTALL_FAILED_USER_RESTRICTED/VERIFICATION_FAILURE, o adb que não executa e o tempo esgotado não falam nisso (adbstate.cpp). (2) Quando a instalação falha, o estado não volta ao anterior: install_main grava Outdated (mesmo se era NoApp) e o estado real só volta quando o poll lê a versão de novo (adb.cpp).; Parte repetida: 'install -r e nunca desinstalar' e 'assinatura diferente dá UPDATE_INCOMPATIBLE e perde o save' já estão em android-migracao-store-operacao. Ficou só uma remissão e o que é novo: o exe instala, a versão vem de um arquivo só, debug e release usam a mesma chave.; Cortei o que só descreve o código (monkey, lista do zip, --apk) e deixei as decisões, as armadilhas e como verificar.
---

# Instalação e atualização do app do celular pelo exe

Quem recebe o zip não tem o app no celular. Por isso o `gbahandoff.exe` instala e atualiza o APK (`GBAHandoff.apk`, que fica ao lado do exe) pelo adb, com o botão Instalar/Atualizar ou a tecla I. Ele usa sempre `adb install -r` e nunca desinstala; o motivo está em android-migracao-store-operacao.

- **A versão fica só no arquivo `VERSION` (`code=N`, `name=X.Y`).** O CMake gera o `version.h` a partir dele e o Gradle lê o mesmo arquivo; os dois param o build se faltar uma chave. **Armadilha:** se você não aumentar o `code`, o exe acha que o celular está em dia (versão instalada >= `code` = Ready) e não oferece a atualização.
- **Debug e release assinam com a mesma chave.** O padrão é `~/.android/debug.keystore`, a chave que já assinou o app no celular do usuário; dá para trocar com `GBAH_KEYSTORE*`. Assim o APK do zip atualiza por cima sem perder o save. Se o release tivesse uma chave própria, a atualização daria UPDATE_INCOMPATIBLE.
- **A versão instalada é lida pouco.** O `dumpsys package` roda uma vez por aparelho que aparece, de novo depois de uma instalação e quando a sessão termina, nunca a cada poll de 2 s. Se o adb falhar (aparelho offline etc.), o resultado não é guardado e a leitura é refeita. "Pacote ausente" conta como resposta válida (NoApp).
- **As mensagens de falha conhecidas** (assinatura diferente, downgrade, falta de espaço, código desconhecido) dizem para NÃO desinstalar, porque desinstalar apaga o save. Alternativa descartada: desinstalar e reinstalar quando a assinatura não bate.
- **Depois de instalar,** o card mostra Ready logo após o sucesso e Outdated logo após a falha, antes de conferir. O valor real só vem no poll seguinte, quando a versão é lida de novo.
- O `tools/package-windows.sh` nunca roda o Gradle. Se o APK de debug não existir, ele só avisa e monta o zip sem o APK; o card então mostra "GBAHandoff.apk nao encontrado".

Como verificar: `ctest -R adbstate_test` (leitura do dumpsys e das mensagens do install). No log, `grep "adb: .* versionCode"`.
