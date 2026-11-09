CC       ?= gcc
LD       ?= ld
OBJCOPY  ?= objcopy
PYTHON   ?= python3

CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin \
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fno-pie -fno-ident -fcf-protection=none -mcmodel=large \
	-maccumulate-outgoing-args -mno-red-zone -Iinclude

BUILD := build
CORE  := build/40hx-core.o

# Built .efi images and their checksums all land here (never the repo root);
# install-efi.sh picks them up from build/efi/ in both build and prebuilt modes.
EFI_DIR := build/efi

# DIRECT_SEC2 booter firmware blobs, self-contained in this project's assets/
# (copies of the v70 .bin files from the 40HX-Unlock-Drive-Patch scheme;
#  provenance only -- the build here never reads outside this directory).
# Symbol targets match build_v70.sh so unlock40x_v70.c logic ports
# line-for-line. objcopy -I binary mangles the SOURCE symbol from the file
# basename (dots -> underscores); we --redefine-sym each to the target
# declared in include/booter_assets.h.
ASSET_DIR := assets
ASSET_OBJS := $(BUILD)/asset_booter_ucode_prod.o \
	$(BUILD)/asset_v67_payload_bin.o \
	$(BUILD)/asset_gsp_bl_tu102.o \
	$(BUILD)/asset_gsp_rm_boot_dbg.o \
	$(BUILD)/asset_fwsec_ga102_bin.o \
	$(BUILD)/asset_fwsec_ga102_sig.o \
	$(BUILD)/asset_fwsec_40hx_prod_bin.o

# $(call asset-rule,<file>,<source-mangled>,<target-symbol>)
define asset_rule
$(BUILD)/asset_$(3).o: $(ASSET_DIR)/$(1) | $(BUILD)
	cd $(ASSET_DIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		--redefine-sym _binary_$(2)_start=$(3) \
		--redefine-sym _binary_$(2)_end=$(3)_end \
		--redefine-sym _binary_$(2)_size=$(3)_size \
		$(1) $(abspath $$@)
	$(OBJCOPY) --rename-section .data=.assets,alloc,load,readonly,data,contents $$@
endef

$(eval $(call asset_rule,booter_ucode_prod.bin,booter_ucode_prod_bin,booter_ucode_prod))
$(eval $(call asset_rule,v67_payload.bin,v67_payload_bin,v67_payload_bin))
$(eval $(call asset_rule,bl_gsp_tu102.bin,bl_gsp_tu102_bin,gsp_bl_tu102))
$(eval $(call asset_rule,gsp_rm_boot_dbg.bin,gsp_rm_boot_dbg_bin,gsp_rm_boot_dbg))
$(eval $(call asset_rule,fwsec_ga102.bin,fwsec_ga102_bin,fwsec_ga102_bin))
$(eval $(call asset_rule,fwsec_ga102_sig.bin,fwsec_ga102_sig_bin,fwsec_ga102_sig))
$(eval $(call asset_rule,fwsec_40hx_prod.bin,fwsec_40hx_prod_bin,fwsec_40hx_prod_bin))

# Optional compile-time default mode for GRUB integration (no LoadOptions):
#   make clean && make                       -> build/efi/NVPermissiveEFI-INSPECT.efi (inspect, safe fallback)
#   make clean && make AUTO=REQUEST_BOOTER   -> build/efi/NVPermissiveEFI-BOOTER.efi  (one-shot unlock)
# The name suffix only documents the intended default; at runtime the mode is
# picked from LoadOptions (the GRUB entry passes 'inspect' / 'booter').
AUTO ?=
CFLAGS += $(if $(AUTO),-DAUTO_MODE=$(AUTO))

ifneq ($(AUTO),)
NAME := $(EFI_DIR)/NVPermissiveEFI-$(subst REQUEST_,,$(AUTO)).efi
else
NAME := $(EFI_DIR)/NVPermissiveEFI-INSPECT.efi
endif

.PHONY: all clean release
all: $(NAME)

# Build both shipped modes plus CHECKSUMS.sha256 into build/efi/ for
# install-efi.sh: the inspect image plus the booter one-shot
# (DIRECT_SEC2 + Gen2 + ReBAR). The mid-way 'clean' drops the objects only
# (each mode needs its own -DAUTO_MODE); build/efi/ images are never touched.
release:
	$(MAKE) clean AUTO=
	$(MAKE) all AUTO=
	$(MAKE) clean AUTO=
	$(MAKE) all AUTO=REQUEST_BOOTER
	mkdir -p $(EFI_DIR)
	cd $(EFI_DIR) && sha256sum NVPermissiveEFI-INSPECT.efi NVPermissiveEFI-BOOTER.efi > CHECKSUMS.sha256

$(EFI_DIR):
	mkdir -p $(EFI_DIR)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/main.o: src/main.c include/uefi_min.h include/core_abi.h include/elf_loader.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/elf_loader.o: src/elf_loader.c include/uefi_min.h include/core_abi.h include/elf64_min.h include/elf_loader.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# DIRECT_SEC2 booter helpers: the one-shot booter flow driven from efi_main
# via booter_run(). The loaders that call these also need the embedded
# asset blobs, which link only here.
$(BUILD)/booter.o: src/booter.c include/booter.h include/booter_regs.h \
		include/uefi_min.h include/core_abi.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# TU106 core: our own SysV .o (replaces the GA102 nvpermissive core)
# mingw64 gcc -c emits COFF; convert to ELF relocatable first (elf_loader
# inspects e_ident/e_type/e_machine), then wrap as binary data.
$(CORE): src/core_40hx.c include/core_abi.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/core_blob.o: $(CORE) | $(BUILD)
	$(OBJCOPY) -I pe-x86-64 -O elf64-x86-64 $(CORE) build/40hx-core.elf.o
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 build/40hx-core.elf.o $@
	$(OBJCOPY) --rename-section .data=.core,alloc,load,readonly,data,contents $@
	$(OBJCOPY) --redefine-sym _binary_build_40hx_core_elf_o_start=_binary_nvpermissive_core_o_start $@
	$(OBJCOPY) --redefine-sym _binary_build_40hx_core_elf_o_end=_binary_nvpermissive_core_o_end $@

# ld writes the image straight into build/efi/ -- one file per mode, no
# intermediate copy in build/.
$(NAME): $(BUILD)/main.o $(BUILD)/elf_loader.o $(BUILD)/booter.o $(BUILD)/core_blob.o $(ASSET_OBJS) | $(EFI_DIR)
	$(LD) -mi386pep --subsystem 10 --entry efi_main --image-base 0x10000000 \
		--file-alignment 0x200 --section-alignment 0x1000 \
		-o $@ $^
	$(PYTHON) tools/verify_efi.py $@

# clean removes intermediate objects only, never build/efi/ (release cleans
# between the two modes and must keep the first image). Drop the released
# images explicitly: rm -rf build/efi
clean:
	rm -f $(BUILD)/main.o $(BUILD)/elf_loader.o $(BUILD)/booter.o $(BUILD)/core_blob.o \
		$(ASSET_OBJS) $(BUILD)/40hx-core.elf.o
	-rm -f $(BUILD)/NVPermissiveEFI*.efi        # legacy: pre-relocation intermediate
	-rm -f NVPermissiveEFI*.efi CHECKSUMS.sha256   # legacy: root-level artifacts
