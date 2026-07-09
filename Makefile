CC      = clang
CFLAGS  = -O2 -Wall -Wextra -std=c99 -Isrc \
          $(shell pkg-config --cflags wayland-client wayland-cursor cairo xkbcommon)

# Read format from config.h so the linker flags match the compiled encoder.
# Defaults to PNG if config.h can't be parsed (cairo handles PNG natively, no extra lib).
_FMT := $(shell awk '/define OPTFORMAT_TYPE/{print $$3}' config.h)

ifeq ($(_FMT),FORMAT_WEBP)
  _EXTRA_LIBS = -lwebp
else ifeq ($(_FMT),FORMAT_JPEG)
  _EXTRA_LIBS = -ljpeg
else
  _EXTRA_LIBS =
endif

LIBS = $(shell pkg-config --libs wayland-client wayland-cursor cairo xkbcommon) \
       $(_EXTRA_LIBS) -lm

PREFIX  = /usr/local
BINDIR  = $(PREFIX)/bin
BINARY  = whot

PROTO_SRCS = \
    src/xdg-shell-protocol.c \
    src/wlr-layer-shell-unstable-v1-protocol.c \
    src/wlr-screencopy-unstable-v1-protocol.c

SRCS = main.c \
       src/wutil.c \
       src/capture.c \
       src/save.c \
       src/scripts.c \
       src/select.c \
       $(PROTO_SRCS)

OBJS = $(SRCS:.c=.o)
DEPS = $(OBJS:.o=.d)

-include $(DEPS)

all: $(BINARY)

$(BINARY): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LIBS)
	strip --strip-unneeded $@

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

protocols:
	wayland-scanner client-header \
	    /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
	    src/xdg-shell-client-protocol.h
	wayland-scanner private-code \
	    /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
	    src/xdg-shell-protocol.c
	wayland-scanner client-header \
	    protocols/wlr-layer-shell-unstable-v1.xml \
	    src/wlr-layer-shell-unstable-v1-client-protocol.h
	wayland-scanner private-code \
	    protocols/wlr-layer-shell-unstable-v1.xml \
	    src/wlr-layer-shell-unstable-v1-protocol.c
	wayland-scanner client-header \
	    protocols/wlr-screencopy-unstable-v1.xml \
	    src/wlr-screencopy-unstable-v1-client-protocol.h
	wayland-scanner private-code \
	    protocols/wlr-screencopy-unstable-v1.xml \
	    src/wlr-screencopy-unstable-v1-protocol.c

install: $(BINARY)
	install -Dm755 $(BINARY) $(DESTDIR)$(BINDIR)/$(BINARY)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BINARY)

clean:
	rm -f $(OBJS) $(DEPS) $(BINARY) src/xdg-shell-protocol.o src/xdg-shell-protocol.d

.PHONY: all protocols install uninstall clean
