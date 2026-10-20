# ROMs de teste

Nenhuma ROM comercial. Somente o que está listado aqui.

## gbah_test.gba (416 bytes)
- Origem: escrita do zero para este projeto, fonte em `gbah_test.s` (ARM). Sem código de terceiros.
- Licença: CC0 / domínio público.
- O que faz: modo 3 (240x160) com padrão que depende do contador de frames e das teclas seguradas; tom de onda quadrada
  (canal 1) com pitch mudando por frame; escreve frame e teclas em SRAM[0..2] a cada frame (SRAM[1] = bits 0-7, SRAM[2] = L/R, bits 8-9) (a string `SRAM_V113` faz o
  mGBA detectar SRAM de 32 KiB). Assim o teste cobre vídeo, áudio, input e save do cartucho.
- O cabeçalho não tem o logo da Nintendo (área zerada); o mGBA não valida. Hardware real recusaria. Checksum do cabeçalho é válido.
- Rebuild: `./build.sh` (precisa de `binutils-arm-none-eabi`; sem root: `apt-get download binutils-arm-none-eabi && dpkg -x *.deb dir`
  e colocar `dir/usr/bin` no PATH). O `.gba` fica versionado para o ctest não depender do assembler.
- O SHA-1 esperado pelo teste é calculado pelo CMake (`file(SHA1 ...)`) como referência independente do SHA-1 do `core/`.

## gbah_test_late.gba (408 bytes)
- Mesma fonte (`gbah_test.s`) montada com `--defsym LATE=1` (o `build.sh` gera as duas).
- Sem a string `SRAM_V113`: o tipo de save fica AUTODETECT até o 1º acesso à SRAM. Só no laço 20 da ROM (~frame 200+ da emulação;
  a ROM leva vários frames de vídeo por laço) grava UM byte (0xA5 em SRAM[0x100]) e depois só lê, misturando-o na imagem.
  Serve para reproduzir o bug da ordem sav/state: importar o .sav antes do state_load num core novo perde o byte e o hash diverge.
- Depois de regravar uma ROM, rode `cmake .` no diretório de build: o SHA-1 esperado é calculado no configure.
