# Compilação a partir de clone novo

Ative ESP-IDF 5.4.4. O Tab5 usa ESP32-P4 como CPU principal.
Forneça os WADs separadamente no microSD, conforme README.md.

```sh
idf.py set-target esp32p4
idf.py build
```

Instale o .bin de build pelo M5Launcher. O Component Manager obtém as dependências.
Não é necessário apagar a flash para instalar pelo launcher.
