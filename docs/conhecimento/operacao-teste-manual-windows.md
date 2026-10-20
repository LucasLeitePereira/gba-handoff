---
titulo: Teste manual do handoff no Windows: roteiro e falsas falhas
tipo: operacao
gerado_por: claude-sonnet-5-5
commit: f84dedf
arquivos: desktop/main.cpp, tools/fetch-adb-windows.sh
revisado_por: claude-opus-5-5
revisado_em: 2026-10-20T12:36:31Z
parecer: Conferi as afirmações em desktop/main.cpp e desktop/handoff.cpp e estão corretas. F2 chama request_return, F5 quick_save, F8 quick_load, F11 alterna a tela cheia e Esc fecha o jogo. A mensagem 'PORTA OCUPADA: outra instancia ja esta aberta' aparece quando server.start falha. O .sav ao lado da ROM só é adotado na primeira importação (added) e não sobrescreve um existente. resume_local recusa com o cabo conectado e mostra 'O celular esta jogando: devolva por la'.; Duplicação: os comandos de build Windows já estão em ambiente-build-teste.md. O fetch-adb-windows.sh e o aviso de que o caminho Windows não tem teste automático já estão em link-operacao.md. Troquei esses trechos por referências.; A lista de teclas é quase só descrição do código. Mantive o mínimo para o roteiro e dei peso às armadilhas que confundem quem testa à mão: o Enter não faz nada com o cabo conectado; arrastar a ROM durante o jogo é recusado; o .sav é adotado uma única vez e sem sobrescrever; a segunda instância mostra PORTA OCUPADA.; Sem segredos nem dados pessoais.
---

# Teste manual do handoff no Windows

O caminho real no Windows (adb de verdade, janela SDL, drag-and-drop) não tem teste automático (ver link-operacao). Para validar, monte o pacote como em ambiente-build-teste e em link-operacao (`adb/` ao lado do `.exe`) e use um celular com depuração USB e o app instalado.

Roteiro: arraste um `.gba` para a janela do menu, abra o jogo e use **F2** para entregar ao celular, **F5**/**F8** para salvar/recarregar o ponto salvo e **Esc** para voltar ao menu.

Comportamentos esperados que parecem bug:
- **Arrastar a ROM durante o jogo** é recusado ("Volte ao menu para importar"). Importe pelo menu.
- **`.sav` ao lado da ROM** (`x.gba.sav` ou `x.sav`) só é adotado na *primeira* importação daquela ROM e nunca sobrescreve um save que já existe. Reimportar com outro `.sav` não muda nada.
- **Depois do F2** ("Jogando no celular"), o Enter só retoma no PC com o **cabo desconectado**. Com o cabo conectado, o PC responde "O celular esta jogando: devolva por la", e o jogo tem que voltar pelo celular.
- **Segunda instância aberta** mostra "PORTA OCUPADA: outra instancia ja esta aberta". A porta TCP é exclusiva, então isso não é falha de rede nem do adb.
