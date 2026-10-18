# CP-500 M80 - emulador. Gera as versoes MS-DOS e Windows.
# A versao ESP32 compila com o Arduino (abra CP5M80.ino) - veja o README.
#
#   make dos   -> CP5M80.EXE  (DJGPP: i586-pc-msdosdjgpp-gcc + CWSDSTUB.EXE do CWSDPMI)
#   make win   -> CP5M80W.EXE (MinGW-w64 / llvm-mingw, 32 bits)
#   make lnx   -> cp5lnx      (plataforma de teste: tela em memoria, teclas por script)

SRC   = main.c dgt.c disk.c common.c kbd.c z80.c roms.c
DJGPP = i586-pc-msdosdjgpp-
MINGW = i686-w64-mingw32-
CWSDSTUB ?= CWSDSTUB.EXE

all: dos win

dos: CP5M80.EXE
win: CP5M80W.EXE
lnx: cp5lnx

CP5M80.EXE: $(SRC) plat_dos.c *.h
	$(DJGPP)gcc -O2 -march=i586 -fomit-frame-pointer -o cp5dos.exe $(SRC) plat_dos.c
	exe2coff cp5dos.exe
	cat $(CWSDSTUB) cp5dos > CP5M80.EXE
	rm -f cp5dos cp5dos.exe

CP5M80W.EXE: $(SRC) plat_win.c *.h cp5m80.rc cp5m80.ico
	$(MINGW)windres cp5m80.rc -O coff -o cp5res.o
	$(MINGW)gcc -O2 -mwindows -static -o CP5M80W.EXE $(SRC) plat_win.c cp5res.o -lwinmm -lgdi32
	rm -f cp5res.o

cp5lnx: $(SRC) plat_lnx.c *.h
	gcc -O2 -o cp5lnx $(SRC) plat_lnx.c

clean:
	rm -f CP5M80.EXE CP5M80W.EXE cp5lnx *.o cp5dos cp5dos.exe

.PHONY: all dos win lnx clean
