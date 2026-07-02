# j2me-ps2 - Makefile (EE / Emotion Engine)
# Compilado dentro do container ps2dev/ps2dev, onde PS2SDK e PS2DEV ja existem.

EE_BIN = j2me.elf

EE_OBJS = src/main.o src/input.o

EE_INCS += -Iinclude

# Caminho de referencia do ps2sdk: libdraw/libgraph/libdma/libpacket + libpad.
# Ordem importa para o linker (dependentes antes das dependencias).
EE_LIBS = -ldraw -lgraph -lpacket -ldma -lmath3d -lpad -lc -lkernel

all: $(EE_BIN)

clean:
	rm -f $(EE_BIN) $(EE_OBJS)

# Regras padrao do ps2sdk
include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
