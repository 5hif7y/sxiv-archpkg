# Maintainer: Lukas Fleischer <lfleischer@archlinux.org>
# Contributor: Bartłomiej Piotrowski <bpiotrowski@archlinux.org>
# Contributor: Thomas Dziedzic < gostrc at gmail >
# Contributor: Bert Muennich <muennich at informatik.hu-berlin.de>
# Contributor: Brad Fanella <bradfanella@archlinux.us>

pkgname=sxiv
pkgver=26
pkgrel=2
pkgdesc='Simple X Image Viewer'
arch=('x86_64')
license=('GPL2')
url='https://github.com/muennich/sxiv'
depends=('imlib2' 'desktop-file-utils' 'xdg-utils' 'hicolor-icon-theme' 'libexif' 'libxft' 'libarchive' 'zlib')
source=("$pkgname-$pkgver.tar.gz"::"https://github.com/muennich/sxiv/archive/v$pkgver.tar.gz"
        "sxiv-archive-support.patch")
sha256sums=('a382ad57734243818e828ba161fc0357b48d8f3a7f8c29cac183492b46b58949'
            '0e30a87f0d5b687a47521810aaaa4548f92b4e49c5f2a88502aef59604eef99d')

prepare() {
  cd "$pkgname-$pkgver"
  patch -Np1 -i "$srcdir/sxiv-archive-support.patch"
  ln -s config.def.h config.h
}

build() {
  make -C "$pkgname-$pkgver"
}

package() {
  cd "$pkgname-$pkgver"
  make PREFIX=/usr DESTDIR="$pkgdir" install
  make -C icon PREFIX=/usr DESTDIR="$pkgdir" install
  install -Dm644 sxiv.desktop "$pkgdir"/usr/share/applications/sxiv.desktop
}
