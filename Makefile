CC      := cc
CFLAGS  := -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
  RAYLIB_LIBS := -lraylib -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo
  CFLAGS      += -I/opt/homebrew/include -I/usr/local/include
  LDFLAGS     += -L/opt/homebrew/lib    -L/usr/local/lib
else
  RAYLIB_LIBS := -lraylib -lGL -lm -lpthread -ldl -lrt -lX11
endif

SRC := main.c editor.c gap_buffer.c syntax.c commands.c theme.c config.c
OBJ := $(SRC:.c=.o)
BIN := notepad

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS) $(RAYLIB_LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean
