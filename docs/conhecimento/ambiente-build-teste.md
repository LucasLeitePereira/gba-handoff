---
titulo: Como montar o ambiente, buildar e testar
tipo: operacao
gerado_por: claude-sonnet-5-5
commit: sem-commit
arquivos: CMakePresets.json, tests/CMakeLists.txt
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T01:06:27Z
parecer: Conferi o documento contra o código e o ambiente. O CMakePresets.json tem os presets build-linux e build-win, e o build-win usa o toolchain mingw-w64. O executável gbahandoff é definido em desktop/CMakeLists.txt, então sai em build-win/desktop/gbahandoff.exe.; A frase 'testes não são buildados em cross-compile' está certa: o CMakeLists.txt raiz tem `if(GBAH_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)`.; A parte do SHA-1 está certa: tests/CMakeLists.txt calcula o hash com file(SHA1) e passa como argumento ao core_test, que compara com gbah_rom_sha1_hex. É uma referência independente do core.; O ambiente bate com o documento: os pacotes apt estão instalados, o SDK tem platforms;android-36, build-tools 36.0.0 e ndk 27.3.13750724, as variáveis estão em ~/.profile, e o ~/.bashrc retorna cedo quando o shell não é interativo (`case $- in ... return`).; Não há documento aprovado parecido. É um procedimento de operação conciso e traz armadilhas que não dá para tirar do código: usar `bash -lc` por causa do ~/.bashrc, não haver testes no cross-compile e o hash de referência ser independente do core.; Não tem segredos nem dados pessoais.
---

# Ambiente, build e testes

- **Pacotes apt**: `cmake ninja-build mingw-w64 openjdk-17-jdk-headless`.
- **Android SDK** em `~/android-sdk`: cmdline-tools, platform-tools, `platforms;android-36`,
  `build-tools;36.0.0`, `ndk;27.3.13750724` (r27d LTS). Variáveis ficam em `~/.profile`:
  rodar com `bash -lc`, pois o `~/.bashrc` retorna cedo em shell não interativo.
- **Linux (dev/testes)**: `cmake --preset build-linux && cmake --build --preset build-linux && ctest --test-dir build-linux`.
- **Windows (cross)**: `cmake --preset build-win && cmake --build --preset build-win`
  → `build-win/desktop/gbahandoff.exe`. Testes não são buildados em cross-compile.
- **Teste de determinismo**: ver `tests/core_test.c`; o ctest passa o SHA-1 de referência da ROM
  (calculado pelo CMake, independente do SHA-1 do core).
