[Compilação de um clone novo](PUBLICATION.md) · [Licenças](THIRD_PARTY_NOTICES.md)

# Tab5UltDoom — Ultimate Doom no M5Stack Tab5

Projeto separado criado como cópia local de Tab5Doom2. Engine doomgeneric,
alvo ESP32-P4, ESP-IDF 5.4.4. Não altera os outros ports.

Coloque o WAD completo de Ultimate Doom em `/doom/DOOM-ultimate.wad` no microSD
FAT32. Também é aceita a raiz `/DOOM-ultimate.wad`, com prioridade para `/doom`.
O nome `DOOM-ULTIMATE.WAD` também é aceito. Nomes longos FAT estão habilitados.
Não há fallback para outro jogo ou para SPIFFS, nem menu de seleção de WADs.

O engine identifica o conteúdo pelo marcador E1M1 e reconhece Ultimate Doom
pela presença de E4M1, habilitando o modo retail e a emulação exe_ultimate.
O quarto episódio, Thy Flesh Consumed, depende de um WAD completo correto;
renomear um WAD de Doom comum não acrescenta esse episódio.

Controles: WASD/setas para mover, Ctrl para atirar, E/espaço para usar,
Aa para correr, Alt + direção para deslocamento lateral, Enter para confirmar,
Esc para menu e 1–7 para armas. Display ST7123/PPA e áudio ES8388 herdados.
Efeitos sonoros a 11025 Hz; música não implementada.

Configurações e saves mantêm o comportamento da base: ficam na pasta do WAD
selecionado. O mod opcional chiquito.wad também é procurado nessa pasta.
A imagem SPIFFS e os artefatos de build da base não foram copiados; este projeto
não gera/grava imagem SPIFFS. A tabela de partições original foi preservada.

## Compilação

```sh
idf.py -B build build
```

Compilação ESP-IDF 5.4.4 concluída: `build/Tab5UltDoom.bin` (883872 bytes),
instalável pelo M5Launcher.
Nenhum flash é realizado automaticamente. A execução de Ultimate Doom e os
quatro episódios ainda precisam de validação física no Tab5.

Histórico técnico herdado: TAB5DOOM_PROGRESS.txt e
TAB5DOOM_DISPLAY_INIT_HISTORY.txt. Documentação da base: README_DOOM2_BASE.md.
Licenças e créditos do engine e componentes permanecem nos respectivos arquivos.

Primeira captura serial confirmou `/sdcard/doom/DOOM-ultimate.wad`, identificação
The Ultimate DOOM e emulação Ultimate Doom, com aproximadamente 36–38 FPS.
A saída causou abort por exit(0) no callback ENDOOM; correção compilada para
executar callbacks e esp_restart. Teste físico da saída corrigida pendente.
