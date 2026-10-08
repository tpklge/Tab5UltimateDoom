# Tab5Doom2 — Doom II no M5Stack Tab5

Este aparelho usa o display **ST7123**, identificado pela versão 3 do controlador
integrado em I2C `0x55`. O reset do touch usa P6 do expansor `0x43`; o LCD P4 é
liberado como entrada com pull-up. A sequência ST7123 completa e os timings
965 Mbps / DPI 70 MHz substituem a tentativa anterior com ST7703.

O DOOM usa a tela em paisagem, com rotação de 90° pelo PPA e proporção preservada.
O teclado original **Tab5 Keyboard** conecta à **Ext.Port1**, em I2C `0x6D`
(SDA GPIO0, SCL GPIO1). Controles:

- **WASD ou setas:** andar e girar.
- **Ctrl:** atirar. **E ou Espaço:** usar/abrir portas.
- **Aa:** correr. **Alt + direção:** deslocamento lateral.
- **Enter:** confirmar. **Esc:** menu. **1–7:** selecionar armas.

O SPIFFS contém `DOOM.WAD` completo com os três episódios. A tabela de
partições reserva 2 MiB para o aplicativo e o restante da flash de 16 MiB
para o SPIFFS em `0x210000`. O carregador aceita nomes de WAD maiúsculos e
minúsculos; há compatibilidade com a barra de status, demos e recursos de menu
dessa edição antiga. O WAD é copiado para PSRAM na inicialização (cerca de cinco
segundos) para evitar leituras aleatórias lentas do SPIFFS durante o jogo.

Esta versão busca exclusivamente **`doom2.wad` no microSD FAT32**. Coloque o
arquivo em **`/doom/doom2.wad`** (preferencial) ou **`/doom2.wad`** na raiz do
cartão. O nome `DOOM2.WAD` também é aceito. A pasta `/doom` tem prioridade.
Quando o WAD está em `/doom`, configurações e saves também ficam nessa pasta;
quando está na raiz, ficam na raiz. O mod opcional `chiquito.wad` deve ficar
junto do WAD escolhido. O serial informa o caminho selecionado. Se o cartão
não montar ou o arquivo não for encontrado, o jogo não inicia e o serial
mostra o erro. Não há fallback para Doom 1 nem para um WAD no SPIFFS.

A imagem SPIFFS descrita acima é herdada da versão Doom 1 e não é utilizada
pelo carregador de Doom II. O CMake ainda gera e inclui essa imagem no flash
completo. Para atualizar somente o aplicativo nesta etapa, com a tabela de
partições já instalada, use os comandos abaixo.
O carregamento de Doom II ainda precisa ser validado no aparelho.

O mixer implementa efeitos sonoros a 11025 Hz; música ainda não é sintetizada.
Os efeitos no alto-falante e a entrada na fase foram confirmados no aparelho.
O amplificador usa P1 do expansor `0x43` em push-pull; o padrão de alta
impedância impedia sua ativação. A inicialização inclui um tom curto de teste.

O serial mostra detecção do teclado e FPS/tempo de PPA a cada cinco segundos.
O touch ST7123 ainda não está implementado como controle do jogo.
Histórico de testes e resultados: `TAB5DOOM_PROGRESS.txt` e
`TAB5DOOM_DISPLAY_INIT_HISTORY.txt`. Logs de diagnóstico ficam em `.debug_logs/`.

Compilar e gravar com ESP-IDF 5.4.4; após cada flash, acompanhar o serial.
Use uma pasta de build nova porque o cache copiado pode referenciar o caminho
anterior do projeto:

```sh
idf.py -B build/doom2 build
idf.py -B build/doom2 -p <porta> app-flash monitor
```

`app-flash` atualiza somente o aplicativo. O comando `flash` completo também
grava o WAD legado do diretório `spiffs`.

Referência histórica da versão Doom 1: para deixar o SPIFFS apagado, a versão
anterior foi gravada manualmente com os comandos abaixo. Essa operação não é
necessária para a alteração do carregador de Doom II:

```sh
esptool.py --chip esp32p4 -p /dev/cu.usbmodem101 erase_flash
esptool.py --chip esp32p4 -p /dev/cu.usbmodem101 write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x2000 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0x10000 build/Tab5Doom.bin
idf.py -p /dev/cu.usbmodem101 monitor
```

O apagamento remove também o NVS. Com o firmware validado, a partição SPIFFS
fica com **13,94 MiB vazios**, e o aplicativo tem **1,13 MiB de folga** dentro
de sua partição. SPIFFS apagado não monta até receber uma imagem ou ser formatado;
o jogo continua carregando do microSD.

O driver ST7123 local deriva do driver Apache-2.0 da Espressif, com a tabela de
inicialização do BSP M5Stack. O mapeamento do teclado deriva do
[M5Unit-KEYBOARD](https://github.com/m5stack/M5Unit-KEYBOARD) (MIT).

---

# ESP32P4Doom
![ESP32-P4](https://img.shields.io/badge/Platform-ESP32--P4-red.svg)
![DOOM](https://img.shields.io/badge/Port-DOOM-orange.svg)
![ESP-IDF](https://img.shields.io/badge/Framework-ESP--IDF%20v5.5-blue.svg)

A complete **independent bare-metal** DOOM port for the **ESP32-P4** microcontroller. Unlike other versions, this project is fully decoupled from official vendor BSP components, implementing its own lean hardware abstraction layer for maximum performance and control.

## Key Improvements
- **Decoupled Architecture**: Removed dependency on `esp32_p4_function_ev_board`. All hardware drivers (I2C, I2S, SDMMC) are implemented natively in `bsp_p4_eval.c`.
- **SD Card WAD Loading**: Full support for loading `.wad` files from a MicroSD card. The system automatically searches both internal SPIFFS and the SD card.
- **PPA Hardware Scaling**: Internal `320x200` rendering upscaled to `1024x600` MIPI DSI display via the ESP32-P4's **Pixel Processing Accelerator**, freeing the CPU for game logic.
- **Micro-Mixer Audio**: A dedicated 16-bit PCM Software Mixer broadcasting over I2S to the onboard ES8311 codec for high-fidelity SFX.
- **USB HID Support**: Direct plug-and-play support for standard USB keyboards.

## Features
- **Project Structure**: Cleaned and renamed entry point to `main.c`.
- **Hardware Abstraction**: Custom `bsp_p4_eval` handles:
  - **I2C**: Touch (GT911) and Audio Codec (ES8311) control.
  - **I2S**: Standard Philips mode for audio data.
  - **SDMMC**: High-speed 4-line SD card interface.
- **Bilingual Cleanliness**: Codebase refactored with all comments and logs standardized to English.

## Game and Mod Loading (WADs)
The engine features an automatic detection system for game files:
- **With SD Card**: The system prioritizes reading from the root of a FAT32 MicroSD card. It looks for base games in this order: `doom2.wad` -> `doom.wad` -> `doom1.wad`.
  - **Chiquito Mod**: If the file `chiquito.wad` is placed on the SD card alongside a base game, the engine will automatically load it as a PWAD (`-file`). This replaces the standard audio with the legendary sounds of Spanish comedian "Chiquito de la Calzada".
- **Without SD Card**: If no SD card is detected, the engine falls back to the internal SPIFFS memory, loading the default DOOM 1 Shareware (`doom1.wad`).

## Missing Features / To-Do
The following features are currently missing from this port:
- **Network** (Multiplayer)
- **Music**

## Requirements
* ESP32-P4-Function-EV-Board (or custom P4 hardware with similar pinout).
* USB Keyboard (connected via USB-H port).

## Build and Flashing
1. **WAD Location**: Place your `.wad` files on the SD card or flash them to the `spiffs` partition.
2. **Compile and Flash**:
```bash
idf.py build flash monitor
```

## About the Developer
Developed and optimized for the ESP32-P4 by **Alejandro Villegas Alonso**.
If you are interested in embedded systems, ESP-IDF development, or want to discuss professional opportunities, feel free to connect!

* 👔 [Alejandro Villegas Alonso on LinkedIn](https://www.linkedin.com/in/alejandro-villegas-alonso-825041b1)

## Credits and Acknowledgements
* **[doomgeneric](https://github.com/ozkl/doomgeneric)**: The portable engine core by *ozkl*.
* **[id Software](https://github.com/id-Software/DOOM)**: The original legends of gaming.
* **[Espressif Systems](https://github.com/espressif)**: For the powerful P4 chip and IDF framework.

## License
Licensed under GPLv2.
