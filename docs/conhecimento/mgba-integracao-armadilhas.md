---
titulo: Armadilhas de integração com a libmgba (áudio, log, ritmo)
tipo: armadilha
gerado_por: claude-sonnet-5-5
commit: sem-commit
arquivos: core/gbah_core.c, desktop/main.cpp
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T01:09:20Z
parecer: Conferi as afirmações em core/gbah_core.c e desktop/main.cpp. Batem com o código: `volume` 0x100 com mCoreInitConfig/mCoreLoadForeignConfig e config só em memória (linhas 122-129), `blip_set_rates` depois do reset (148-157), fila de ~60 ms e 59,7275 Hz (main.cpp:147,152) e o logger global (linha 108). As contas também batem: 60/59,7275 ≈ 0,46% a mais.; Item de log impreciso: quem filtra é o callback próprio `quiet_log`, não o `mLogSetDefaultLogger`, que só o instala. Os erros que sobram vão para o stderr, não para o stdout. Corrigido.; O item sobre `ldrh/strh` em ARM é fato genérico do conjunto de instruções e não tem relação com a libmgba nem com os arquivos citados (é de tests/roms/gbah_test.s). Removido porque foge do assunto do documento; se valer, deve virar um documento à parte sobre a ROM de teste.; Faltava uma armadilha real do código: depois de `state_load` é preciso `blip_clear`, porque as amostras no buffer são de outra linha do tempo. Adicionada.; O documento explica o porquê, é conciso, não duplica nada aprovado e não tem segredos.
---

# Armadilhas de integração com a libmgba

- **Áudio mudo**: sem `volume` explícito na config em memória, o som sai mudo. A solução é usar `mCoreInitConfig` + `mCoreConfigSetDefaultIntValue("volume", 0x100)` + `mCoreLoadForeignConfig`.
  Não use `mCoreLoadConfig`: ele lê o perfil do usuário, e o resultado deixa de ser reprodutível.
- **`blip_set_rates` depois do reset**: o reset recria os blips e perde as taxas. Drene o áudio a cada frame, senão o blip (buffer de 2048 amostras) descarta amostras.
- **`blip_clear` depois de `state_load`**: as amostras que já estavam no buffer são de outra linha do tempo; se ficarem, sai um trecho de som errado.
- **Ritmo**: o GBA roda a 59,7275 Hz. Com vsync de 60 Hz, o jogo corre 0,46% mais rápido. Por isso o desktop usa a fila de áudio como relógio (mantém ~60 ms enfileirados) e só cai para um timer quando não há áudio.
- **Log**: o logger padrão do mGBA faz printf de tudo no stdout, inclusive lixo do jogo (ex.: "Invalid DMA source address" a cada `state_load`). Instalamos um logger próprio com `mLogSetDefaultLogger`, que só passa ERROR/FATAL e manda para o stderr. Ele é global: vale para o processo inteiro, inclusive para outros usos do mGBA.
