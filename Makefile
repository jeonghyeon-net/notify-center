PREFIX     ?= $(HOME)/.local
BINDIR     ?= $(PREFIX)/bin
DOCDIR     ?= $(PREFIX)/share/doc/notify-center
LICENSEDIR ?= $(PREFIX)/share/licenses/notify-center

CFLAGS ?= -O2 -Wall
PKGS    = gtk+-3.0 x11

notify-center: notify-center.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS) $(shell pkg-config --cflags --libs $(PKGS))

install: notify-center
	install -Dm755 notify-center       $(DESTDIR)$(BINDIR)/notify-center
	install -Dm755 notify-badge        $(DESTDIR)$(BINDIR)/notify-badge
	install -Dm755 notify-center-setup $(DESTDIR)$(BINDIR)/notify-center-setup
	install -Dm644 README.md           $(DESTDIR)$(DOCDIR)/README.md
	install -Dm644 LICENSE             $(DESTDIR)$(LICENSEDIR)/LICENSE

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/notify-center $(DESTDIR)$(BINDIR)/notify-badge \
		$(DESTDIR)$(BINDIR)/notify-center-setup \
		$(DESTDIR)$(DOCDIR)/README.md $(DESTDIR)$(LICENSEDIR)/LICENSE
	-rmdir $(DESTDIR)$(DOCDIR) $(DESTDIR)$(LICENSEDIR) 2>/dev/null

clean:
	rm -f notify-center

.PHONY: install uninstall clean
