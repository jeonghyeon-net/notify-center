PREFIX    ?= $(HOME)/.local
BINDIR    ?= $(PREFIX)/bin
DATADIR   ?= $(PREFIX)/share
AUTOSTART ?= $(HOME)/.config/autostart

CFLAGS ?= -O2 -Wall
PKGS    = gtk+-3.0 x11

notify-center: notify-center.c
	$(CC) $(CFLAGS) -o $@ $< $(shell pkg-config --cflags --libs $(PKGS))

install: notify-center
	install -Dm755 notify-center $(DESTDIR)$(BINDIR)/notify-center
	install -Dm755 notify-badge  $(DESTDIR)$(BINDIR)/notify-badge
	install -d $(DESTDIR)$(DATADIR)/dbus-1/services $(DESTDIR)$(AUTOSTART)
	sed 's|@BINDIR@|$(BINDIR)|g' data/org.freedesktop.Notifications.service.in \
		> $(DESTDIR)$(DATADIR)/dbus-1/services/org.freedesktop.Notifications.service
	sed 's|@BINDIR@|$(BINDIR)|g' data/notify-center.desktop.in > $(DESTDIR)$(AUTOSTART)/notify-center.desktop
	install -m644 data/xfce4-notifyd.desktop $(DESTDIR)$(AUTOSTART)/xfce4-notifyd.desktop

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/notify-center $(DESTDIR)$(BINDIR)/notify-badge \
		$(DESTDIR)$(DATADIR)/dbus-1/services/org.freedesktop.Notifications.service \
		$(DESTDIR)$(AUTOSTART)/notify-center.desktop $(DESTDIR)$(AUTOSTART)/xfce4-notifyd.desktop

clean:
	rm -f notify-center

.PHONY: install uninstall clean
