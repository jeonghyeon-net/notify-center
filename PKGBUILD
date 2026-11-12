# Maintainer: Jeonghyeon Kim
pkgname=notify-center
pkgver=0.1.1
pkgrel=1
pkgdesc='Notification server for Xfce (X11) with a persistent, mobile-style notification list'
arch=('x86_64' 'aarch64')
url='https://github.com/jeonghyeon-net/notify-center'
license=('MIT')
depends=('gtk3' 'libx11' 'glib2' 'systemd')
makedepends=('git' 'pkgconf')
optdepends=('xfce4-genmon-plugin: panel bell button with unread count'
            'xfconf: automatic panel button and shortcut setup')
provides=('notification-daemon')
install=notify-center.install
source=("$pkgname-src::git+$url.git#tag=v$pkgver")
sha256sums=('SKIP')

build() {
  cd "$pkgname-src"
  make
}

package() {
  cd "$pkgname-src"
  make PREFIX=/usr DESTDIR="$pkgdir" install
}
