BUILD_DIR=build
include $(N64_INST)/include/n64.mk

src = m64k.c tlb.c testsuite.c dynrec.c
asm = m64k_asm.S
assets_xm = $(wildcard assets/*.xm)
assets_wav = $(wildcard assets/*.wav)
assets_png = $(wildcard assets/*.png)

assets_conv = $(addprefix filesystem/,$(notdir $(wildcard assets/*.btest)))

# Activate more accurate configuration for testsuite
M64K_CONFIG += -DM64K_CONFIG_TIMING_ACCURACY=0
M64K_CONFIG += -DM64K_CONFIG_ADDRERR=1
M64K_CONFIG += -DM64K_CONFIG_PRIVERR=1
M64K_CONFIG += -DM64K_CONFIG_DIVBYZERO=1
M64K_CONFIG += -DM64K_CONFIG_ADDR_WRAP=1
# Enable the inline fast paths so the testsuite exercises them (with ADDRERR=1
# they additionally cover the bail-to-generic odd-address route).
M64K_CONFIG += -DM64K_FASTPATHS
# M64K_PREDECODE (killed 2026-08-07, kept opt-in): the phase-1a record
# dispatch was testsuite-green (125/126) but regressed -5.5..-5.9 fps in
# every bucket — the per-PC record stream adds ~20+ cyc/insn of dcache
# misses, more than phase-2 specialization could recover. Enable here to
# re-test the machinery if the design is ever revisited.
# M64K_CONFIG += -DM64K_PREDECODE
# m64k dynarec phase-1 machinery (arena + empty block-table probes): opt-in
# via make ... DYNREC_ON=1 so the testsuite covers the probe-instrumented
# dispatch. (The EPC-range extension lives in the game's hw_n64.S and is
# proven by the game-side boot [DYNTEST].)
ifdef DYNREC_ON
M64K_CONFIG += -DM64K_DYNREC
endif

N64_CFLAGS += $(M64K_CONFIG)
N64_ASFLAGS += $(M64K_CONFIG)

all: m64k_testsuite.z64

filesystem/%: assets/%
	@mkdir -p $(dir $@)
	@echo "    [DATA] $@"
	@cp "$<" "$@"
	@$(N64_BINDIR)/mkasset -c 1 -o filesystem "$@"

$(BUILD_DIR)/m64k_testsuite.dfs: $(assets_conv)
$(BUILD_DIR)/m64k_testsuite.elf: $(src:%.c=$(BUILD_DIR)/%.o) $(asm:%.S=$(BUILD_DIR)/%.o)

m64k_testsuite.z64: N64_ROM_TITLE="M64K Testsuite"
m64k_testsuite.z64: $(BUILD_DIR)/m64k_testsuite.dfs

emu: m64k_testsuite.z64
	$(N64EMU) m64k_testsuite.z64

run: m64k_testsuite.z64
	g64drive upload m64k_testsuite.z64 && g64drive debug

clean:
	rm -rf $(BUILD_DIR) m64k_testsuite.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
