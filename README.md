# CP-500 M80 — emulador para Windows, MS-DOS e ESP32

Emulador do **Prológica CP-500 M80**, o TRS-80 Model III nacional com modo
CP/M de 80 colunas. Roda o BASIC da ROM, o DOS500 e o SO-08 (CP/M 2.2 em 80x24).

- Z80 a 2 MHz, vídeo 64x16 (BASIC/DOS500) e 80x24 (CP/M)
- 64K de RAM (48K no BASIC/DOS500, 64K no CP/M, como na máquina real)
- Controladora WD1793 com 4 drives; discos DMK, DSK (JV1/JV3), HFE, IMD e IMG, com leitura e gravação
- Logotipo de partida e "BASIC (S ou N)", como no aparelho
- Teclado ABNT2/US com editor de teclado personalizado; CapsLock = SHIFT+0 do CP-500
- Setas do CP/M originais ou no padrão WordStar (opção no F5)
- LEDs de acesso aos drives: na tela (Windows/DOS) ou LEDs reais (ESP32)
- ROMs embutidas: nada mais precisa ser baixado

## Plataformas

| Versão | Arquivo | Requisitos |
|---|---|---|
| Windows | `CP5M80W.EXE` | Windows 10/11, arquivo único |
| MS-DOS | `CP5M80.EXE` | DOS puro ou FreeDOS, 486/Pentium, VGA, 4 MB (CWSDPMI embutido) |
| ESP32 | `CP5M80.bin` | TTGO VGA32 v1.4 (ESP32 + PSRAM), teclado PS/2, monitor VGA, cartão SD |

Os executáveis prontos estão em [Releases](../../releases).

## Teclas

| Tecla | Função |
|---|---|
| F1 | ajuda |
| F5 | configuração |
| F9 | discos (montar, ejetar, criar vazio, exportar) |
| F10 | reset |
| Shift+F10 | sair (no ESP32, reinicia a placa) |
| Esc | BREAK (ESC no CP/M) |
| PgUp/PgDn | CLEAR |

## Compilar

Todo o código está em `CP5M80/`, compartilhado pelas três plataformas.
Apenas a camada `plat_*.c` muda.

**MS-DOS** — [DJGPP](https://github.com/andrewwutw/build-djgpp) e o `CWSDSTUB.EXE` do
[CWSDPMI](http://sandmann.dotster.com/cwsdpmi/):

    cd CP5M80
    make dos CWSDSTUB=/caminho/CWSDSTUB.EXE

**Windows** — MinGW-w64 ou llvm-mingw (32 bits):

    make win

**ESP32** — Arduino IDE ou arduino-cli:

- core **esp32 2.0.17**
- biblioteca **FabGL 1.0.9**
- placa "ESP32 Dev Module" com PSRAM **Enabled**, Partition Scheme **Huge APP**, Flash Mode **DIO** e Flash Freq **40 MHz**
- abra `CP5M80/CP5M80.ino` e compile. Os arquivos `plat_dos.c`, `plat_win.c` e `plat_lnx.c` são ignorados automaticamente no ESP32.

**Teste em Linux** — `make lnx` gera `cp5lnx`, que tem a tela em memória e
recebe teclas por script. Serve para testar sem hardware.

## Estrutura

    CP5M80/
      z80.c, z80.h       núcleo Z80 (baseado no superzazu/z80, MIT)
      dgt.c, dgt.h       máquina: memória, mapas do M80, portas, WD1793, vídeo, teclado
      disk.c, disk.h     formatos de disco (DMK, JV1, JV3, HFE, IMD, IMG)
      kbd.c, kbd.h       teclado do PC -> matriz do CP-500
      main.c             laço principal, menus F1/F5/F9, configuração
      common.c/.h        utilitários
      roms.c             ROMs 27128 e gerador de caracteres 2732
      plat_*.c/.cpp      DOS, Windows, ESP32, Linux (teste)
    docs/                LEIAME de cada plataforma (Windows, MS-DOS, ESP32)

## Notas técnicas

Veja `docs/LEIAME_WINDOWS.TXT` (ou o de outra plataforma). O documento descreve:

- o mapa de memória do M80, selecionado pelos bits A8-A15 do `IN (F4)`;
- o vídeo de 80 colunas em páginas de 1K;
- o fim de setor do WD1793 por NMI;
- a troca da parte alta da 27128 na partida;
- como a BIOS do SO-08 lê o teclado.

## Créditos

RetroLabs — JL Martins e Luiz Pacheco, 2026.

CP-500 é marca da Prológica. As ROMs são do equipamento original e
estão incluídas para fins de preservação.
