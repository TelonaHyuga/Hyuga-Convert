# Maintainer: Telona Hyuga <your-email@example.com>
pkgname=hyuga-convert
pkgver=1.0.0
pkgrel=1
pkgdesc="GTK3 GUI media converter with optional Nautilus integration"
arch=('x86_64' 'aarch64')
url="https://github.com/TelonaHyuga/Hyuga-Convert"
license=('GPL-3.0-or-later')
depends=(
  'gtk3'
  'ffmpeg'
  'glib2'
)
makedepends=(
  'glib2'      # glib-compile-resources
  'nautilus'   # libnautilus-extension-4 для плагина
)
optdepends=(
  'nautilus: for the Nautilus file-manager integration'
)

# GitHub-архив распаковывается в Hyuga-Convert-$pkgver,
# а не в $pkgname-$pkgver — поэтому храним имя отдельно.
_ghname="Hyuga-Convert"

source=("$pkgname-$pkgver.tar.gz::$url/archive/refs/tags/$pkgver.tar.gz")
sha256sums=('efc8d6cc20bbb7c65788496b9748885a26be04267b4157215ec18c11b2847087')

build() {
  cd "$_ghname-$pkgver"
  make PREFIX=/usr
}

package() {
  cd "$_ghname-$pkgver"
  make PREFIX=/usr DESTDIR="$pkgdir" install
}
