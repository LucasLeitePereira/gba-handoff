---
titulo: Operação do link: empacotar o adb (Windows) e limites dos testes
tipo: operacao
gerado_por: claude-sonnet-5-5
commit: sem-commit
arquivos: tools/fetch-adb-windows.sh, tests/CMakeLists.txt, tests/link_integration_test.c, tests/fake_adb.sh, .gitignore
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T01:59:34Z
parecer: Conferi as afirmações no código e elas estão corretas. tools/fetch-adb-windows.sh copia os 4 arquivos para <dest>/adb/, usa build-win/desktop como padrão, aceita ADB_ZIP_SHA256 e imprime o hash. O .gitignore tem adb/. O tests/CMakeLists.txt cria proto_test_san só com GNU/Clang e link_integration_test só com GBAH_BUILD_DESKTOP AND UNIX. tests/link_integration_test.c define SDL_VIDEO_DRIVER, SDL_AUDIO_DRIVER e XDG_DATA_HOME e confere o reverse de FAKE123 ignorando o UNAUTH9. desktop/proc.cpp tem um ramo _WIN32.; Não duplica os outros documentos: o ambiente-build-teste.md fala só do ctest e do cross-compile, sem empacotar o adb, sem a licença e sem as variáveis para rodar sem display.; Cortei a lista de testes e a descrição passo a passo das sessões do link_integration_test, porque só resumem o que o código e os comentários dele já dizem. Fiquei com o que não sai do código: o porquê do NOTICE, como verificar o hash, as variáveis necessárias para rodar o gbahandoff sem display e a lacuna de cobertura no Windows.
---

# Operação do link: adb no pacote e limites dos testes

- **Empacotar o adb (Windows):** `tools/fetch-adb-windows.sh [dest]` (padrão `build-win/desktop`) baixa o platform-tools oficial e põe `adb.exe`, `AdbWinApi.dll`, `AdbWinUsbApi.dll` e `NOTICE.txt` em `<dest>/adb/`, ao lado do `.exe`. Os binários **não são versionados** (`adb/` no `.gitignore`). A licença do adb permite redistribuir desde que o aviso vá junto, então o `NOTICE.txt` **tem que estar no pacote**.
- **Fixar o zip:** o script imprime o sha256 do zip baixado. Com `ADB_ZIP_SHA256=<hex>`, ele recusa um zip diferente. Use isso para builds reproduzíveis, porque a URL é sempre a `latest`.
- **Rodar o `gbahandoff` sem display (WSL/CI):** defina `SDL_VIDEO_DRIVER=dummy` e `SDL_AUDIO_DRIVER=dummy`. Defina também `XDG_DATA_HOME` como um diretório temporário, senão o device_id é gravado no `$HOME`. O `link_integration_test` já faz isso sozinho; só precisa definir na mão fora dele.
- **Lacuna de cobertura:** o `link_integration_test` (binário real + `tests/fake_adb.sh`) só roda em POSIX com o desktop habilitado. O build Windows é cross-compilado e não é executado, então a parte de `desktop/proc.cpp` para Windows (o lançamento do adb) não tem teste automático: valide numa máquina Windows de verdade.
