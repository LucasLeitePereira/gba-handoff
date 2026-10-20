# GBA Handoff

Emulador de Game Boy Advance para **Android e Windows** com troca de aparelho no estilo Nintendo Switch:
você joga no celular, pluga o cabo USB e o jogo continua no PC do mesmo ponto — e volta para o celular
quando quiser. Os saves dos dois lados ficam sincronizados, e o celular pode servir de controle para o PC.

Baseado no núcleo do [mGBA](https://mgba.io).

> **Não inclui jogos.** Use apenas ROMs de cartuchos que você possui.

## Como usar (Windows + Android)
1. Baixe o `GBAHandoff-windows.zip` na página de [Releases](../../releases) e descompacte.
2. No celular: ative as *Opções do desenvolvedor* e a **Depuração USB**.
3. Plugue o celular no PC e abra o `gbahandoff.exe`. Aceite a depuração no celular.
4. Clique em **Instalar** no exe: ele instala (ou atualiza) o app no celular.
5. Jogue. Ao plugar o cabo, o jogo vai para o PC; no PC, **Esc** abre o menu do jogo.

**Nunca desinstale o app do celular para atualizar** — desinstalar apaga os saves. O exe atualiza por cima.

## Compilar
Requisitos: CMake ≥ 3.25, Ninja, um compilador C/C++ (e `mingw-w64` para gerar o exe do Windows a partir
do Linux), JDK 17 e Android SDK/NDK r27 para o app.

```sh
git clone --recursive https://github.com/LucasLeitePereira/gba-handoff
cd gba-handoff
cmake --preset build-linux && cmake --build --preset build-linux && ctest --preset build-linux
cmake --preset build-win   && cmake --build --preset build-win      # gbahandoff.exe
(cd android && ./gradlew assembleDebug)                               # APK
tools/package-windows.sh                                              # dist/GBAHandoff-windows.zip
```

## Licenças
- Este projeto: [MPL-2.0](LICENSE).
- [mGBA](https://github.com/mgba-emu/mgba): MPL-2.0. [SDL](https://github.com/libsdl-org/SDL): zlib.
- `adb` (Android SDK Platform-Tools), distribuído no pacote do Windows: Apache-2.0 — ver `adb/NOTICE.txt`.
- ROMs de teste em `tests/roms/`: escritas para este projeto, CC0.
