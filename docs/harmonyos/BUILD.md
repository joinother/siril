# Building SIRIL for HarmonyOS PC

The port keeps SIRIL's own source tree and build system.  Nothing in the
engine is rewritten; a new meson option (`-Dohos_bridge=true`) adds a shared
library that embeds the headless engine and exposes it to the ArkTS UI and the
TCP control port.

## 1. Toolchain

| Item | Value used for the verified build |
| --- | --- |
| DevEco Studio | 26.0.0 (`/Applications/DevEco-Studio.app`) |
| Native SDK | `sdk/default/openharmony/native`, clang 15.0.4, sysroot API 26 |
| Target | `aarch64-linux-ohos` (`arm64-v8a`) |
| Device | HUAWEI MateBook `HAD-W32`, HarmonyOS 7.0.0, API 26 |

Cross files used by the build live outside the repository (build root):

* `ohos-cross.ini` — meson cross file (`--target=aarch64-linux-ohos`, sysroot,
  pkg-config paths, CMake toolchain hook);
* `ohos-cmake-toolchain.cmake` — minimal CMake toolchain for meson CMake
  subprojects (wcslib, htmesh, healpix, librtprocess, yyjson) and for OpenCV.

## 2. Cross-compiled dependencies

All of them are built into one prefix (`$SIRIL_OHOS_PREFIX`, default
`../siril-ohos-build/sysroot`) with the same clang/sysroot:

| Library | Version | Notes |
| --- | --- | --- |
| glib / gio / gobject | 2.82.4 | meson, `-Dtests=false -Dintrospection=disabled` |
| pcre2 | 10.44 | 8-bit shared only |
| expat | 2.6.4 | |
| fftw | 3.3.10 | float precision, threads |
| gsl | 2.8 | autotools, library links manually |
| cfitsio | 4.5.0 | `--disable-curl` |
| wcslib | 8.2 | CMake subproject, cfitsio-backed |
| lcms2 | 2.16 | autotools |
| libpng | 1.6.45 | 16-bit support |
| libjpeg-turbo | 3.1.0 | no SIMD |
| libtiff | 4.7.0 | jpeg + zlib only |
| exiv2 | 0.28.5 | `-DEXIV2_ENABLE_INIH=OFF` |
| libraw | 0.21.3 | no OpenMP |
| json-glib | 1.10.6 | |
| freetype / fontconfig | 2.13.3 / 2.15.0 | |
| pixman / fribidi | 0.44.2 / 1.0.16 | |
| harfbuzz | 10.1.0 | glib + freetype backends |
| cairo | 1.18.2 | png + fontconfig |
| pango | 1.54.0 | |
| gdk-pixbuf | 2.42.12 | `-Dgio_sniffing=false` |
| opencv | 4.10.0 | `core,imgproc,calib3d,features2d,flann,stitching` |

Disabled in the engine: GTK4, OpenMP, libcurl, sqlite, libheif, ffms2, ffmpeg,
libjxl, libXISF, libgit2.  The command catalog still exposes all 218 commands
of this build; the disabled formats/online features simply report
"unsupported" when used.

## 3. Engine

```bash
meson setup build-ohos ../siril-src \
  --cross-file ohos-cross.ini --prefix="$SIRIL_OHOS_PREFIX" \
  -Dgtk=false -Dohos_bridge=true -Drelocatable-bundle=yes \
  -Dopenmp=false -Dlibcurl=false -Dsqlite=false -Dlibheif=false \
  -Dffms2=false -Dffmpeg=false -Dlibjxl=false -DlibXISF=false -Dlibgit2=false \
  -Dlibtiff=true -Dlibjpeg=true -Dlibpng=true -Dexiv2=true -Dlibraw=true \
  -Db_staticpic=true
ninja src/libsiril-ohos.so
```

`-Drelocatable-bundle=yes` makes SIRIL resolve its data files from
`$APPDIR/usr/share/siril`; the HarmonyOS application sets `APPDIR` to its
sandbox and copies `data/*.csv` there on first launch.

## 4. HAP

```bash
scripts/build-ohos-app.sh native   # engine
scripts/build-ohos-app.sh hap      # ArkTS + NAPI + libs + data
```

`build-ohos-app.sh hap` collects the ELF dependency closure
(`scripts/collect-ohos-libs.py`), copies `data/*.csv` and the generated
`commands.json` into `rawfile/siril-data`, then runs hvigor
(`assembleHap --mode module -p product=default -p buildMode=debug`).

## 5. Signing and installation

Debug signing uses AGC-issued material (certificate type 1 + debug profile
bound to the device UDID):

```bash
java -jar $OH/toolchains/lib/hap-sign-tool.jar sign-app \
  -keyAlias sirildebug -signAlg SHA256withECDSA -mode localSign \
  -appCertFile cert.cer -profileFile profile.p7b \
  -inFile entry-default-unsigned.hap -keystoreFile key.p12 \
  -outFile SIRIL-signed.hap -keyPwd "$PASS" -keystorePwd "$PASS"

hdc install -r SIRIL-signed.hap
hdc shell aa start -a EntryAbility -b com.joinother.siril
```

Signing material and the p12 password live in `harmonyos/.signing/` and are
ignored by git.

## 6. Notes and known limits

* Only `libsiril-ohos.so` is built for the device; the desktop `siril-cli`
  executable links network code that the curl-less headless build disables.
* HarmonyOS closes the application shortly after its window leaves the
  foreground; the CLI tests therefore restart it (`aa force-stop` +
  `aa start`) before each suite.
* The control port listens on `127.0.0.1` inside the app sandbox and is
  reached from the host through `hdc fport`.
