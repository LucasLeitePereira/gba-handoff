---
titulo: Armadilhas de build - mGBA, SDL3 estático e mingw-w64
tipo: armadilha
gerado_por: claude-sonnet-5-5
commit: sem-commit
arquivos: CMakeLists.txt, desktop/CMakeLists.txt, cmake/mingw-w64.cmake
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T01:06:50Z
parecer: Conferi os três primeiros itens no código e estão corretos. third_party/mgba/CMakeLists.txt:14-16 calcula LIBMGBA_ONLY com get_directory_property(EXCLUDE_FROM_ALL), lido no momento em que entra no próprio diretório. Isso confirma que o EXCLUDE_FROM_ALL tem que estar no add_subdirectory. Ajustei o texto para dizer o mecanismo, em vez de um vago 'em outro lugar', e citei a alternativa: definir LIBMGBA_ONLY explicitamente.; O item do SDL3 confere com desktop/CMakeLists.txt, onde SDL_UNIX_CONSOLE_BUILD só é ativado em UNIX AND NOT APPLE. O do mingw-w64 confere com cmake/mingw-w64.cmake (compiladores -posix e flags -static).; Tirei o item de WSL2. Não tem relação com os arquivos citados, não é armadilha de build e não dá para conferir no repositório. Além disso, 'C: não está montado' descreve esta máquina, não o projeto. Se for útil, deve virar um documento à parte do tipo operacao.; Tirei o tamanho do .exe (~3,6 MB): é um número que muda a cada build e o código não confirma. Troquei por uma forma de verificar (objdump) que continua valendo.; Encurtei o texto e deixei o sintoma de cada armadilha, que é o que os comentários do código não trazem. Não há segredos nem dados pessoais.
---

# Armadilhas de build

- **mGBA**: o modo LIBMGBA_ONLY (só lib estática, sem frontends e sem dependências) não é uma opção. O CMake do mGBA o deduz com `get_directory_property(EXCLUDE_FROM_ALL)` do próprio diretório, assim que entra nele. Por isso o `EXCLUDE_FROM_ALL` precisa estar no `add_subdirectory(third_party/mgba EXCLUDE_FROM_ALL)`: definir a propriedade depois, ou nos targets, chega tarde. Se não estiver lá, o configure tenta buildar frontends e procurar dependências opcionais. A alternativa é definir `LIBMGBA_ONLY` antes do `add_subdirectory`.
- **SDL3 estático no Linux sem headers de X11/Wayland**: o configure falha com "could not find X11 or Wayland". Solução: `SDL_UNIX_CONSOLE_BUILD=ON` (só drivers dummy). Por isso o build Linux serve apenas para dev/testes, sem janela.
- **mingw-w64**: o `x86_64-w64-mingw32-g++` padrão é a variante `-win32`, que não tem `std::thread`/`std::mutex`, e a compilação falha nesses headers. Use os compiladores `-posix` com `-static -static-libgcc -static-libstdc++` para o .exe não depender de libgcc, libstdc++ e libwinpthread em DLL. Para conferir: `x86_64-w64-mingw32-objdump -p gbahandoff.exe | grep 'DLL Name'` deve listar só DLLs do sistema.
