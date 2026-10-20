---
titulo: Armadilhas de build do Android (NDK, AAudio, 16 KB, Gradle)
tipo: armadilha
gerado_por: claude-sonnet-5-5
commit: sem-commit
arquivos: android/app/build.gradle.kts, android/app/src/main/cpp/CMakeLists.txt, android/app/src/main/cpp/jni_bridge.c, android/gradle.properties
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T01:33:15Z
parecer: Conferi as afirmações no código e elas batem. Em build.gradle.kts: minSdk 26, NDK 27.3.13750724, buildToolsVersion 36.0.0, cmake 3.31.5 e -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON. No CMakeLists.txt do app: set(CMAKE_BUILD_TYPE Release) como variável normal, pool de 3 compilações, add_subdirectory da raiz com desktop e testes em OFF, e __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__. Em jni_bridge.c:125 a chamada a setUsage está guardada com __builtin_available(android 28, *). O gradle.properties tem -Xmx1g, daemon=false, parallel=false e workers.max=2.; Rodei readelf nos .so já buildados: arm64-v8a e x86_64 mostram LOAD com alinhamento 0x4000 e armeabi-v7a mostra 0x1000, como o documento diz. O cmake do sistema é o 3.25.1.; Não duplica nada: ambiente-build-teste.md lista as versões de build-tools e NDK, mas não explica por que estão fixadas nem quais armadilhas cada uma evita.; Ajuste: o item de memória estava confuso ('em máquina folgada dá para afrouxar, mas passe também --no-daemon'). O comentário em gradle.properties manda passar --no-daemon sempre, e o motivo é a máquina de build de 3 GB. Reescrevi o item com base nisso e enxuguei o texto.
---

# Armadilhas de build do Android

- **AAudio `setUsage` é API 28+, o minSdk é 26.** Sintoma: erro de compilação "unavailable: introduced
  in Android 28". Solução: `-D__ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__` no target nativo e guardar a
  chamada com `__builtin_available(android 28, *)` (o símbolo vira fraco e não derruba o app em 26/27).
- **NDK r27 não alinha segmentos ELF em 16 KB por padrão** (o r28 alinha). Sem
  `-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON` a lib não serve a dispositivos de página 16 KB. Verificar:
  `readelf -lW libgbahandoff.so | grep LOAD` deve mostrar alinhamento `0x4000` em arm64-v8a e x86_64.
  O armeabi-v7a continua em `0x1000` (4 KB); é o esperado, não um erro.
- **Build type**: o AGP passa `CMAKE_BUILD_TYPE=Debug` nas variantes debug, mas o CMake do app
  força `Release` com uma variável normal (que sombreia o cache). Intencional: emulador em -O0 fica
  injogável, então até o APK debug leva código nativo otimizado. Não "conserte" isso.
- **Reuso do CMake raiz via `add_subdirectory`** (com desktop e testes desligados): evita duplicar as
  flags do mGBA, que precisam ser idênticas nas duas plataformas.
- **Build-tools e cmake fixados**: sem `buildToolsVersion`, o Gradle baixa a versão que o AGP prefere
  e o build passa a depender de rede e da máquina. O `cmake` do SDK (3.31.5) também é fixado, porque o
  cmake do sistema (3.25) é mais antigo e não é o que o AGP deve usar.
- **Memória**: a máquina de build tem 3 GB de RAM. Por isso `android/gradle.properties` usa heap pequeno
  (`-Xmx1g`), sem daemon, sem paralelismo e com `workers.max=2`, e o CMake limita a 3 compilações
  simultâneas. Rode o Gradle sempre com `--no-daemon` também na linha de comando, como pede o
  comentário do arquivo. Esses limites podem ser afrouxados em máquinas com mais RAM.
