TARGET = bleem2.elf
OBJS = src/main.o src/mem.o src/gpu.o src/cpu.o src/spu.o src/input.o src/vmu.o src/cdrom.o src/hle.o src/jit2.o src/gte.o src/dma.o src/timers.o src/sio.o src/memcard.o src/log.o src/display.o

all: rm-elf $(TARGET)

include ../KallistiOS/Makefile.rules

KOS_CFLAGS += -O2 -Wall -Wno-unused-variable -ffast-math

clean:
	-rm -f $(TARGET) $(OBJS)

rm-elf:
	-rm -f $(TARGET)

$(TARGET): $(OBJS)
	kos-cc -o $(TARGET) $(OBJS)

cdi: $(TARGET)
	mkdcdisc64 -e $(TARGET) -d psbios -f "build/Metal Slug X (USA).bin" -o build/bleem2.cdi

run: $(TARGET)
	$(KOS_LOADER) $(TARGET)