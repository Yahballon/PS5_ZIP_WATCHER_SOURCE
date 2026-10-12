# Build the PS5 .elf with the ps5-payload-dev SDK; use "make host" for Linux testing.
.DEFAULT_GOAL := all
ifndef PS5_PAYLOAD_SDK
  ifneq ($(filter host clean,$(MAKECMDGOALS)),)
  else
    $(error Set PS5_PAYLOAD_SDK to the SDK installation directory, or run make host)
  endif
else
  include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
endif

ELF := ps5_zip_watcher.elf
MINIZ := third_party/miniz
MINIZ_SOURCES := $(MINIZ)/miniz.c $(MINIZ)/miniz_tdef.c $(MINIZ)/miniz_tinfl.c

.PHONY: all host clean
all: $(ELF)

$(ELF): zip_watcher.c $(MINIZ_SOURCES)
	$(CC) -O2 -Wall -Wextra -DPS5_TARGET -D_FILE_OFFSET_BITS=64 -DMINIZ_NO_ARCHIVE_APIS -I$(MINIZ) -o $@ $^

host:
	$(HOST_CC) -D_FILE_OFFSET_BITS=64 -O2 -Wall -Wextra -std=c11 -DSETTLE_SECONDS=0 -o ps5_zip_watcher_host zip_watcher.c -lz

HOST_CC ?= cc
clean:
	rm -f $(ELF) ps5_zip_watcher_host
