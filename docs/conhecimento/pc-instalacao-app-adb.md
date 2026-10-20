---
titulo: Instalação e atualização do app do celular pelo exe (VERSION, chave única, consulta de versão)
tipo: decisao
gerado_por: claude-sonnet-5-5
commit: a4c2c43
arquivos: desktop/adb.cpp, desktop/adbstate.cpp, desktop/adbstate.h, CMakeLists.txt, VERSION, android/app/build.gradle.kts, tools/package-windows.sh
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T19:32:09Z
parecer: Conferido no código (commit a4c2c43): android/app/build.gradle.kts define o signingConfig "handoff". O storeFile vem de GBAH_KEYSTORE e, sem ele, usa ~/.android/debug.keystore. GBAH_KEYSTORE_PASS, GBAH_KEY_ALIAS e GBAH_KEY_PASS têm por padrão android, androiddebugkey e android, que são os valores do debug keystore. Debug e release usam getByName("handoff"). Isso bate com o texto novo.; desktop/adb.cpp, adbstate.cpp, adbstate.h e CMakeLists.txt não mudaram entre 2199055 e a4c2c43. As afirmações já conferidas na versão aprovada continuam válidas. Em tools/package-windows.sh só mudou a pasta onde o zip é montado: ele continua sem rodar o Gradle e continua avisando e montando o zip quando falta o APK de debug.; O que é novo e não está em outro documento: (1) o nome do signingConfig e os padrões das variáveis GBAH_*; (2) como conferir o certificado com apksigner verify --print-certs antes de distribuir, comparando com um APK anterior ou com keytool. Isso evita mandar um APK que daria UPDATE_INCOMPATIBLE nos celulares que já têm o app. Nenhum outro documento aprovado traz esse procedimento (grep por apksigner/keytool em docs/conhecimento não achou nada).; O resto é igual à versão aprovada, que este documento substitui. Não há duplicação nova, explica o porquê e não tem segredos: as senhas android/androiddebugkey são o padrão público do debug keystore do Android.
---

# Instalação e atualização do app do celular pelo exe

Quem recebe o zip não tem o app no celular. Por isso o `gbahandoff.exe` instala e atualiza o APK (`GBAHandoff.apk`, que fica ao lado do exe) pelo adb, com o botão Instalar/Atualizar ou a tecla I. Ele usa sempre `adb install -r` e nunca desinstala; o motivo está em android-migracao-store-operacao.

- **A versão fica só no arquivo `VERSION` (`code=N`, `name=X.Y`).** O CMake gera o `version.h` a partir dele e o Gradle lê o mesmo arquivo; os dois param o build se faltar uma chave. **Armadilha:** se você não aumentar o `code`, o exe acha que o celular está em dia (versão instalada >= `code` = Ready) e não oferece a atualização.
- **Debug e release assinam com a mesma chave (`signingConfig` "handoff" no Gradle).** O padrão é `~/.android/debug.keystore`, a chave que já assinou o app no celular do usuário; dá para trocar com `GBAH_KEYSTORE` (e `GBAH_KEYSTORE_PASS`, `GBAH_KEY_ALIAS`, `GBAH_KEY_PASS`, que por padrão são os do debug keystore). Assim o APK do zip atualiza por cima sem perder o save. Se o release tivesse uma chave própria, a atualização daria UPDATE_INCOMPATIBLE.
- **Antes de distribuir um APK, confira o certificado:** `apksigner verify --print-certs app-debug.apk` (build-tools do SDK) e compare o digest com o de um APK anterior ou com `keytool -list -v -keystore ~/.android/debug.keystore`. Foi assim que se confirmou que a chave nova é igual à antiga. Se o digest mudar, o `install -r` falha nos celulares que já têm o app (UPDATE_INCOMPATIBLE).
- **A versão instalada é lida pouco.** O `dumpsys package` roda uma vez por aparelho que aparece, de novo depois de uma instalação e quando a sessão termina, nunca a cada poll de 2 s. Se o adb falhar (aparelho offline etc.), o resultado não é guardado e a leitura é refeita. "Pacote ausente" conta como resposta válida (NoApp).
- **As mensagens de falha conhecidas** (assinatura diferente, downgrade, falta de espaço, código desconhecido) dizem para NÃO desinstalar, porque desinstalar apaga o save. Alternativa descartada: desinstalar e reinstalar quando a assinatura não bate.
- **Depois de instalar,** o card mostra Ready logo após o sucesso e Outdated logo após a falha, antes de conferir. O valor real só vem no poll seguinte, quando a versão é lida de novo.
- O `tools/package-windows.sh` nunca roda o Gradle. Se o APK de debug não existir, ele só avisa e monta o zip sem o APK; o card então mostra "GBAHandoff.apk nao encontrado".

Como verificar: `ctest -R adbstate_test` (leitura do dumpsys e das mensagens do install). No log, `grep "adb: .* versionCode"`.
