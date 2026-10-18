#!/bin/bash
# Build the installer and uninstaller packages (unsigned) into build/.
# Run via `make pkg`, which builds and tests the filter first.
set -euo pipefail
cd "$(dirname "$0")/.."
VERSION=${VERSION:?set VERSION}
B=build
ROOT=$B/pkgroot

rm -rf "$ROOT" "$B"/*.pkg
install -d "$ROOT/usr/libexec/cups/filter" "$ROOT/Library/Printers/PPDs/Contents/Resources"
install -m 755 "$B/rastertotspl" "$ROOT/usr/libexec/cups/filter/rastertotspl"
install -m 644 ppd/*-open-source.ppd "$ROOT/Library/Printers/PPDs/Contents/Resources/"

# --ownership recommended installs everything as root:wheel, which CUPS
# requires before it will run a filter.
pkgbuild --root "$ROOT" --scripts pkg/scripts --ownership recommended \
  --identifier io.github.brossow.polono-driver --version "$VERSION" \
  --install-location / "$B/polono-driver-component.pkg"

res=$B/resources; rm -rf "$res"; cp -R pkg/resources "$res"; cp LICENSE "$res/LICENSE.txt"
sed "s/__VERSION__/$VERSION/" pkg/distribution.xml > "$B/distribution.xml"
productbuild --distribution "$B/distribution.xml" --resources "$res" \
  --package-path "$B" "$B/Polono-Driver-$VERSION.pkg"

pkgbuild --nopayload --scripts pkg/uninstall-scripts \
  --identifier io.github.brossow.polono-driver.uninstall --version "$VERSION" \
  "$B/polono-uninstall-component.pkg"
sed "s/__VERSION__/$VERSION/" pkg/uninstall-distribution.xml > "$B/uninstall-distribution.xml"
productbuild --distribution "$B/uninstall-distribution.xml" --resources pkg/uninstall-resources \
  --package-path "$B" "$B/Uninstall-Polono-Driver-$VERSION.pkg"

rm -rf "$ROOT" "$res" "$B"/*distribution.xml "$B"/polono-*-component.pkg
ls -l "$B"/*.pkg
