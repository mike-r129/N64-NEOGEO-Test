.PHONY: all clean help mvs64 mvs64-clean pctest pctest-clean

V ?= 0
D ?= 0

help:
	@echo "make mvs64 ROM=<game.zip> BIOS=<bios.rom>:  Build the mvs64 ROM"
	@echo "make pctest:   Build the PC reference emulator (SDL2)"
	@echo
	@echo "Use make <target> D=1     to generate debugging symbols"
	@echo "Use make <target> V=1     to generate verbose output"

all: mvs64 pctest

mvs64:
	@echo "Building mvs64"
	@make -f Makefile.mvs64 D=$(D) V=$(V) ROM=$(ROM) BIOS=$(BIOS)

mvs64-clean:
	@echo "Cleaning mvs64"
	@make -f Makefile.mvs64 clean ROM=dummy BIOS=dummy

pctest:
	@echo "Building pctest"
	@make -f Makefile.pctests D=$(D) V=$(V)

pctest-clean:
	@echo "Cleaning pctest"
	@make -f Makefile.pctests clean

clean: mvs64-clean pctest-clean
