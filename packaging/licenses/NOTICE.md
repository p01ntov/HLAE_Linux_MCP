# Bundled third-party components

CS2 Camera Studio uses Qt, Qt for Python (PySide6 and Shiboken), and
demoparser2. The upstream copyright notices, license texts and source locations
are preserved in this directory. Other bundled components retain their notices
in the neighbouring directories. `bundled-distributions.json` identifies the
Python packages used for this particular build.

Qt for Python 6.10.3 package metadata offers
`LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only`. The LGPL and both GPL texts are
included under `upstream/qt-for-python-6.10.3/LICENSES`. Qt Base, Qt SVG and Qt
Wayland also use per-file license choices and contain third-party code with its
own terms. Their original license texts, attribution records and referenced
license files are retained under the corresponding `upstream/qt-*` directories.
These upstream inventories also describe optional components that this package
may not contain. Retaining Qt's commercial-license reference does not grant a
commercial license or replace the applicable open-source terms.

The demoparser2 0.42.0 MIT license is preserved verbatim under
`upstream/demoparser2-0.42.0/LICENSE`, including the placeholder copyright line
present in that upstream release. No substitute author or year is invented.

`provenance.json` records exact release commits, original download URLs and
SHA-256 hashes for the copied upstream documents. The package builder checks
these hashes and rejects a changed dependency version until the notice sources
are reviewed and updated. These records document packaging provenance; they do
not certify every possible redistribution scenario.

## Source and build information

- Qt for Python 6.10.3: <https://code.qt.io/cgit/pyside/pyside-setup.git/tag/?h=v6.10.3>,
  source commit `863e6b6e6269fbd401db06e2066119b9014fcf0d`.
  Build instructions: <https://doc.qt.io/qtforpython-6/building_from_source/index.html>.
- Qt Base 6.10.3: <https://github.com/qt/qtbase/tree/7ddbc87d8e14ce51d2957ea72d0a6077593d5ff4>.
- Qt SVG 6.10.3: <https://github.com/qt/qtsvg/tree/77765f981c067cec8f250e13688e5be75ee94a1b>.
- Qt Wayland 6.10.3: <https://github.com/qt/qtwayland/tree/e07f1168b2d65ee191c5220ad14a524f6fa1f64c>.
- Qt build instructions: <https://doc.qt.io/qt-6/build-sources.html>.
- demoparser2 0.42.0: <https://github.com/LaihoE/demoparser/tree/d3767705dc5846d73ed29db50eaeda58778dc934>.

The application bundles shared Qt libraries from the installed PySide6 wheel.
It does not statically link the application into Qt. The application's
`tools/build_live_appimage.py` and `packaging/live/requirements-build.txt`
describe its packaging process. Upstream source links are supplied above;
the complete upstream source archives are not embedded in the AppImage.

## Replacing the shared libraries

Extract the AppImage in a directory you own:

```sh
./CS2-Camera-Studio-x86_64.AppImage --appimage-extract
```

The extracted files can be modified. Qt shared libraries and plugins are under
`squashfs-root/usr/bin/_internal/PySide6/Qt/`; the bindings and Shiboken shared
libraries are under `squashfs-root/usr/bin/_internal/PySide6/` and
`squashfs-root/usr/bin/_internal/shiboken6/`. Replace the relevant libraries with
compatible builds, preserving the directory layout, SONAMEs, Python ABI and
required Qt plugin interfaces. Launch the extracted application with:

```sh
APPDIR="$PWD/squashfs-root" "$PWD/squashfs-root/AppRun"
```

The launcher does not verify or reject replacement Qt libraries. The package's
SHA-256 metadata identifies the original artifact; it is not an execution gate
for an extracted modified copy. Rebuilding the AppImage is optional. No
additional restriction on modifying these libraries, or reverse engineering
to debug such modifications, is imposed by this notice. The bundled license
texts govern their use.
