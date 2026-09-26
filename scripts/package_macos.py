"""Build the downloadable lapis for macOS: lapis.app in a signed, notarized DMG.

The official Qt binaries leave Vulkan out and Homebrew's Qt needs macOS 26 and a
dozen more libraries, so the first step builds Qt from its pinned source with
Vulkan on and everything else bundled (arm64, macOS 14 and later).

    uv run --no-project python scripts/package_macos.py qt        # once, ~30 min
    uv run --no-project python scripts/package_macos.py app       # lapis.app
    uv run --no-project python scripts/package_macos.py dmg       # signed DMG
    uv run --no-project python scripts/package_macos.py notarize --profile NAME
    uv run --no-project python scripts/package_macos.py all --profile NAME
    uv run --no-project python scripts/package_macos.py release --tag v0.1.0

Signing uses the keychain's Developer ID Application identity (or
LAPIS_SIGN_IDENTITY). Notarizing uses a notarytool keychain profile, made once
with `xcrun notarytool store-credentials NAME`.
"""

import argparse
import ctypes
import getpass
import hashlib
import json
import os
import plistlib
import re
import shutil
import signal
import socket
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from lapis import SetupError, ghostty_prefix  # noqa: E402

RELEASE = ROOT / "build" / "release"
DOWNLOADS = RELEASE / "downloads"
SOURCES = RELEASE / "sources"
QT_PREFIX = RELEASE / "qt"
# The prefix compiled into QtCore. Qt finds its files relative to itself at
# run time, so this only has to be neutral: never a path on this machine.
QT_RUNTIME_PREFIX = "/opt/lapis-qt"
QT_VERSION = "6.11.2"
QT_URL = "https://download.qt.io/official_releases/qt/6.11/6.11.2/submodules"
QT_MODULES = {
    "qtbase": "5b2e00eccaf5a4d8c14134ffa0ea8dfd0a35ae1ffc7f8d87fa4305a1ed23cf22",
    "qtshadertools": "805046b8b7757665586890b375940047e874ae3ab00adb6d3f2b38fc6b200b1c",
    "qtdeclarative": "215b7b70517e380123eabc6b92243f3c47b6f016a91d126057dbe53551c6b430",
}
VULKAN_HEADERS = Path("/opt/homebrew/opt/vulkan-headers/include")
# The app carries this MoltenVK (Apache-2.0, system frameworks only) as its
# Vulkan library; Qt's Cocoa plugin also compiles against its headers.
MOLTENVK = Path("/opt/homebrew/opt/molten-vk")
MACOS_TARGET = "14.0"
ARCHITECTURE = "arm64"
# Leave cores for whoever is using the Mac while Qt builds.
JOBS = max(2, (os.cpu_count() or 4) - 4)
VULKAN_INCLUDE = RELEASE / "vulkan-headers" / "include"
MOLTENVK_LIBRARY = MOLTENVK / "lib" / "libMoltenVK.dylib"
APP_BUILD = RELEASE / "lapis-build"
# Sparkle updates the downloaded app from the appcast on the latest release.
SPARKLE_VERSION = "2.10.0"
SPARKLE_SHA256 = "c2bf58aa8387266ac179357b1415d6f2635f044da8be41042af32425dae6da0c"
SPARKLE_URL = (
    "https://github.com/sparkle-project/Sparkle/releases/download/"
    f"{SPARKLE_VERSION}/Sparkle-{SPARKLE_VERSION}.tar.xz"
)
SPARKLE = RELEASE / "sparkle"
APPCAST = RELEASE / "appcast.xml"
RELEASES = "https://github.com/RESMP-DEV/lapis/releases"
APP = RELEASE / "stage" / "lapis.app"
APP_ZIP = RELEASE / "lapis-app.zip"
DMG = RELEASE / "lapis-macos-arm64.dmg"
ENTITLEMENTS = ROOT / "apps/desktop/macos/lapis.entitlements"
NOTICES = {
    "lapis-LICENSE.txt": ROOT / "LICENSE",
    "Qt-NOTICES.txt": ROOT / "third_party/qt/NOTICES.txt",
    "MoltenVK-NOTICES.txt": ROOT / "third_party/moltenvk/NOTICES.txt",
    "Ghostty-NOTICES.txt": ROOT / "third_party/ghostty/NOTICES.txt",
    "Sparkle-NOTICES.txt": ROOT / "third_party/sparkle/NOTICES.txt",
}
# lapis never asks for camera, contacts and the like through Qt.
UNUSED_PLUGINS = ("permissions",)
# The headless host (login helper, phone host) draws nothing.
EXTRA_PLUGINS = ("platforms/libqoffscreen.dylib",)
MACHO_MAGIC = {
    b"\xcf\xfa\xed\xfe",
    b"\xce\xfa\xed\xfe",
    b"\xca\xfe\xba\xbe",
    b"\xbe\xba\xfe\xca",
}
ALLOWED_LIBRARY_PREFIXES = (
    "@rpath/",
    "@executable_path/",
    "@loader_path/",
    "/System/Library/",
    "/usr/lib/",
)
PHONE_ICON = ROOT / "apps/ios/Lapis/Assets.xcassets/AppIcon.appiconset/icon.png"
MAC_ICON = ROOT / "apps/desktop/macos/lapis.icns"
# The phone's square artwork on macOS's icon grid: an 824-point rounded
# square with continuous corners, centred on a 1024 canvas, with a soft shadow.
# The phone's gem fills 59% of its tile, which reads small in the Dock, so the
# artwork is enlarged 1.3 times about the gem's centre (52% down) first.
ICON_SCRIPT = """
import math
import sys
from PIL import Image, ImageDraw, ImageFilter
source, target = sys.argv[1], sys.argv[2]
canvas, body, oversample = 1024, 824, 4
half = body * oversample / 2
# A superellipse (exponent 5) approximates Apple's continuous corners.
points = []
for step in range(2048):
    angle = 2 * math.pi * step / 2048
    c, s = math.cos(angle), math.sin(angle)
    points.append((half + half * math.copysign(abs(c) ** 0.4, c),
                   half + half * math.copysign(abs(s) ** 0.4, s)))
mask = Image.new("L", (body * oversample, body * oversample), 0)
ImageDraw.Draw(mask).polygon(points, fill=255)
mask = mask.resize((body, body), Image.LANCZOS)
art = Image.open(source).convert("RGBA")
side = art.width / 1.3
left, top = (art.width - side) / 2, art.height * 0.52 - side / 2
art = art.crop((round(left), round(top), round(left + side), round(top + side)))
art = art.resize((body, body), Image.LANCZOS)
art.putalpha(mask)
offset = (canvas - body) // 2
icon = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
shade = Image.new("RGBA", (body, body), (0, 0, 0, 0))
shade.putalpha(mask.point(lambda value: value * 90 // 255))
icon.paste(shade, (offset, offset + 10), shade)
icon = icon.filter(ImageFilter.GaussianBlur(14))
icon.paste(art, (offset, offset), art)
icon.save(target)
"""


class PackageError(RuntimeError):
    """A step that cannot continue; the message says what to fix."""


def run(command, **kwargs):
    print("$ " + " ".join(str(part) for part in command), flush=True)
    return subprocess.run([str(part) for part in command], check=True, **kwargs)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch(name, expected):
    """Download one Qt module's source once and check it against its pin."""
    archive = DOWNLOADS / f"{name}-everywhere-src-{QT_VERSION}.tar.xz"
    if not archive.exists() or sha256(archive) != expected:
        DOWNLOADS.mkdir(parents=True, exist_ok=True)
        partial = archive.with_suffix(".part")
        print(f"Downloading {archive.name}", flush=True)
        with urllib.request.urlopen(f"{QT_URL}/{archive.name}") as response:
            with partial.open("wb") as stream:
                shutil.copyfileobj(response, stream)
        if sha256(partial) != expected:
            partial.unlink()
            raise PackageError(f"{archive.name} does not match its pinned SHA-256")
        partial.rename(archive)
    source = SOURCES / f"{name}-everywhere-src-{QT_VERSION}"
    if not source.exists():
        SOURCES.mkdir(parents=True, exist_ok=True)
        with tarfile.open(archive) as bundle:
            bundle.extractall(SOURCES, filter="data")
    return source


def common_cmake_arguments():
    """Settings every Qt module and lapis itself build with."""
    # Paths in __FILE__ and debug maps become relative, so no build path (and
    # no user name) is compiled into what ships.
    remap = f"-ffile-prefix-map={RELEASE}=. -ffile-prefix-map={ROOT}=."
    return [
        "-G",
        "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_OSX_ARCHITECTURES={ARCHITECTURE}",
        f"-DCMAKE_OSX_DEPLOYMENT_TARGET={MACOS_TARGET}",
        "-DCMAKE_C_COMPILER=/usr/bin/clang",
        "-DCMAKE_CXX_COMPILER=/usr/bin/clang++",
        "-DCMAKE_OBJCXX_COMPILER=/usr/bin/clang++",
        f"-DCMAKE_C_FLAGS={remap}",
        f"-DCMAKE_CXX_FLAGS={remap}",
        f"-DCMAKE_OBJCXX_FLAGS={remap}",
        # Nothing from Homebrew may be found and linked.
        "-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local",
        "-DCMAKE_SYSTEM_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local",
        "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF",
    ]


def qt_module_arguments():
    return common_cmake_arguments() + [
        f"-DCMAKE_INSTALL_PREFIX={QT_RUNTIME_PREFIX}",
        f"-DCMAKE_STAGING_PREFIX={QT_PREFIX}",
        "-DQT_BUILD_EXAMPLES=OFF",
        "-DQT_BUILD_TESTS=OFF",
        "-DQT_GENERATE_SBOM=ON",
    ]


QTBASE_FEATURES = {
    "vulkan": "ON",
    # Bundled copies, so the app carries no other library.
    "system_pcre2": "OFF",
    "system_harfbuzz": "OFF",
    "system_freetype": "OFF",
    "system_png": "OFF",
    "system_jpeg": "OFF",
    "system_doubleconversion": "OFF",
    "system_libb2": "OFF",
    "system_textmarkdownreader": "OFF",
    # Not used by lapis.
    "pkg_config": "OFF",
    "dbus": "OFF",
    "glib": "OFF",
    "icu": "OFF",
    "openssl": "OFF",
    "zstd": "OFF",
    "brotli": "OFF",
    "gssapi": "OFF",
    "libproxy": "OFF",
    "sql": "OFF",
    "printsupport": "OFF",
    "widgets": "OFF",
    "testlib": "OFF",
}
QTDECLARATIVE_FEATURES = {
    # lapis uses the Basic style.
    "quickcontrols2_fusion": "OFF",
    "quickcontrols2_imagine": "OFF",
    "quickcontrols2_material": "OFF",
    "quickcontrols2_universal": "OFF",
    "quickcontrols2_macos": "OFF",
    "quickcontrols2_ios": "OFF",
    "quickcontrols2_windows": "OFF",
    "qml_debug": "OFF",
}


def build_module(name, source, features=None, extra=()):
    stamp = QT_PREFIX / f".{name}-{QT_VERSION}"
    if stamp.exists():
        print(f"{name} {QT_VERSION} already built", flush=True)
        return
    build = RELEASE / "qt-build" / name
    options = [f"-DFEATURE_{key}={value}" for key, value in (features or {}).items()]
    run(
        ["cmake", "-S", source, "-B", build]
        + qt_module_arguments()
        + options
        + list(extra)
    )
    run(["nice", "-n", "10", "cmake", "--build", build, "--parallel", str(JOBS)])
    run(["cmake", "--install", build])
    stamp.touch()


def command_qt(_arguments):
    if sys.platform != "darwin":
        raise PackageError("The downloadable app is built on macOS")
    if not (VULKAN_HEADERS / "vulkan" / "vulkan.h").exists():
        raise PackageError("Vulkan headers are missing: brew install vulkan-headers")
    if not (MOLTENVK / "include" / "MoltenVK" / "mvk_vulkan.h").exists():
        raise PackageError("MoltenVK is missing: brew install molten-vk")
    # Only the Vulkan and MoltenVK headers, never the rest of Homebrew's
    # include folder.
    headers = RELEASE / "vulkan-headers" / "include"
    for part, origin in (
        ("vulkan", VULKAN_HEADERS),
        ("vk_video", VULKAN_HEADERS),
        ("MoltenVK", MOLTENVK / "include"),
    ):
        shutil.rmtree(headers / part, ignore_errors=True)
        shutil.copytree(origin / part, headers / part)
    sources = {name: fetch(name, digest) for name, digest in QT_MODULES.items()}
    vulkan = [f"-DVulkan_INCLUDE_DIR={headers}"]
    build_module("qtbase", sources["qtbase"], QTBASE_FEATURES, vulkan)
    # Modules on top of QtGui see its Vulkan API only with the same headers.
    after_base = [f"-DCMAKE_PREFIX_PATH={QT_PREFIX}"] + vulkan
    build_module("qtshadertools", sources["qtshadertools"], extra=after_base)
    build_module(
        "qtdeclarative",
        sources["qtdeclarative"],
        QTDECLARATIVE_FEATURES,
        after_base,
    )
    print(f"Qt {QT_VERSION} with Vulkan is in {QT_PREFIX}", flush=True)


def command_icon(_arguments):
    """Remake apps/desktop/macos/lapis.icns from the phone's icon."""
    work = RELEASE / "icon"
    shutil.rmtree(work, ignore_errors=True)
    iconset = work / "lapis.iconset"
    iconset.mkdir(parents=True)
    master = work / "lapis-1024.png"
    run(
        ["uv", "run", "--no-project", "--with", "pillow>=11", "python", "-c"]
        + [ICON_SCRIPT, PHONE_ICON, master]
    )
    for points in (16, 32, 128, 256, 512):
        for factor, suffix in ((1, ""), (2, "@2x")):
            pixels = points * factor
            name = iconset / f"icon_{points}x{points}{suffix}.png"
            run(
                ["sips", "-z", pixels, pixels, master, "--out", name],
                stdout=subprocess.DEVNULL,
            )
    run(["iconutil", "--convert", "icns", "--output", MAC_ICON, iconset])
    print(f"Wrote {MAC_ICON.relative_to(ROOT)}", flush=True)


KHRONOS = "https://raw.githubusercontent.com/KhronosGroup"
MOLTENVK_COMPONENTS = (
    # Name, revision, license, license file. The revisions are the ones
    # MoltenVK 1.4.2 builds with (its ExternalRevisions folder).
    ("MoltenVK", "v1.4.2", "Apache-2.0", f"{KHRONOS}/MoltenVK/v1.4.2/LICENSE"),
    (
        "SPIRV-Cross",
        "6c09849fe88c48eaed08413aa022aaa136a3a057",
        "Apache-2.0",
        f"{KHRONOS}/SPIRV-Cross/6c09849fe88c48eaed08413aa022aaa136a3a057/LICENSE",
    ),
    (
        "SPIRV-Tools",
        "0d6fd73ca73830ccab5fa1f00ed5ed40124e2c55",
        "Apache-2.0",
        f"{KHRONOS}/SPIRV-Tools/0d6fd73ca73830ccab5fa1f00ed5ed40124e2c55/LICENSE",
    ),
    (
        "SPIRV-Headers",
        "29981f65241605e08b0ede4cfeb999fe3b723c6a",
        "MIT-Khronos-old",
        f"{KHRONOS}/SPIRV-Headers/29981f65241605e08b0ede4cfeb999fe3b723c6a/LICENSE",
    ),
    (
        "Vulkan-Headers",
        "e3b1eec08173d6b825cd3ac88c885a63b621504a",
        "Apache-2.0 OR MIT",
        f"{KHRONOS}/Vulkan-Headers/e3b1eec08173d6b825cd3ac88c885a63b621504a/LICENSE.md",
    ),
    (
        "cereal",
        "a56bad8bbb770ee266e930c95d37fff2a5be7fea",
        "BSD-3-Clause",
        "https://raw.githubusercontent.com/USCiLab/cereal/"
        "a56bad8bbb770ee266e930c95d37fff2a5be7fea/LICENSE",
    ),
)
# Qt's command-line tools only run while building; nothing of them ships.
QT_BUILD_ONLY_LICENSE = "Qt-GPL-exception-1.0"
QT_MODULE_LICENSE = (
    "LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only"
)


def spdx_packages(path):
    """Name, version, license and copyright of each package in a tag-value SBOM."""
    packages, current, key, lines = [], {}, None, None
    for line in path.read_text().splitlines():
        if lines is not None:
            lines.append(line)
            if "</text>" in line:
                current[key] = "\n".join(lines)
                lines = None
            continue
        tag, separator, value = line.partition(": ")
        if not separator:
            continue
        if tag == "PackageName":
            current = {"name": value}
            packages.append(current)
        elif value.startswith("<text>") and "</text>" not in value:
            key, lines = tag, [value]
        else:
            current[tag] = value
    for package in packages:
        for field, value in package.items():
            package[field] = value.replace("<text>", "").replace("</text>", "").strip()
    return packages


def license_ids(expression):
    words = expression.replace("(", " ").replace(")", " ").split()
    return {word for word in words if word not in {"AND", "OR", "WITH"}}


def qt_notices():
    """Qt's own license, then every component its SBOM says the build holds."""
    modules, components = [], {}
    for module in ("qtbase", "qtdeclarative"):
        for package in spdx_packages(
            QT_PREFIX / "sbom" / f"{module}-{QT_VERSION}.spdx"
        ):
            expression = package.get("PackageLicenseConcluded", "NOASSERTION")
            name = package["name"]
            if (
                expression == "NOASSERTION"
                or QT_BUILD_ONLY_LICENSE in expression
                or name.startswith("Bootstrap")
            ):
                continue
            if expression == QT_MODULE_LICENSE:
                modules.append(name)
                continue
            label = name.split("_Attribution_")[-1]
            label = label.removeprefix("Bundled").removesuffix("Private")
            version = package.get("PackageVersion", "unknown")
            copyright_text = package.get("PackageCopyrightText", "")
            components.setdefault((version, expression, copyright_text), label)
    used = {"LGPL-3.0-only", "GPL-3.0-only"}
    for version, expression, _ in components:
        used |= license_ids(expression)
    lines = [
        f"Qt {QT_VERSION} notices",
        "=" * len(f"Qt {QT_VERSION} notices"),
        "",
        "lapis.app contains Qt frameworks and plugins built without modification",
        f"from the official Qt {QT_VERSION} source archives (SHA-256):",
        "",
    ]
    lines += [
        f"  {QT_URL}/{name}-everywhere-src-{QT_VERSION}.tar.xz" for name in QT_MODULES
    ]
    lines += [f"    {digest}" for digest in QT_MODULES.values()]
    lines += [
        "",
        "The lapis release that carries this file attaches the same archives, and",
        "scripts/package_macos.py in the lapis repository is the exact build",
        "configuration. Qt is used under the GNU Lesser General Public License",
        "version 3 (LGPL-3.0-only); its texts follow. The Qt libraries are",
        "separate, dynamically loaded files in lapis.app/Contents/Frameworks and",
        "Contents/PlugIns, and you may replace them with your own build of Qt.",
        "Where a third-party component below offers a choice, lapis takes the",
        "first license listed other than GPL-2.0 or GPL-3.0.",
        "",
        "Qt modules in this build: " + ", ".join(sorted(set(modules))),
        "",
        "Third-party components inside those Qt libraries",
        "-------------------------------------------------",
    ]
    for (version, expression, copyright_text), label in sorted(
        components.items(), key=lambda item: item[1].lower()
    ):
        lines += ["", f"{label} {version}", f"License: {expression}", copyright_text]
    for identifier in sorted(used - {"LicenseRef-Qt-Commercial"}):
        text = next(
            (
                path.read_text(errors="replace")
                for module in ("qtbase", "qtdeclarative")
                if (
                    path := SOURCES
                    / f"{module}-everywhere-src-{QT_VERSION}"
                    / "LICENSES"
                    / f"{identifier}.txt"
                ).exists()
            ),
            None,
        )
        if text is None:
            raise PackageError(f"No text for license {identifier} in the Qt sources")
        lines += ["", "", f"[{identifier}]", "", text.rstrip()]
    return "\n".join(lines) + "\n"


def moltenvk_notices():
    lines = [
        "MoltenVK notices",
        "================",
        "",
        "lapis.app/Contents/Frameworks/libMoltenVK.dylib is Homebrew's unmodified",
        "molten-vk 1.4.2 build (MoltenVK from KhronosGroup, Apache-2.0), which",
        "compiles in the components below at the revisions MoltenVK pins.",
        "SHA-256 of Homebrew's file, before lapis sets its install name and signs it:",
        f"  {sha256(MOLTENVK_LIBRARY)}",
    ]
    for name, revision, expression, url in MOLTENVK_COMPONENTS:
        with urllib.request.urlopen(url) as response:
            text = response.read().decode()
        lines += [
            "",
            "",
            f"{name} {revision}",
            f"License: {expression}",
            url,
            "",
            text.rstrip(),
        ]
    return "\n".join(lines) + "\n"


def command_notices(_arguments):
    """Regenerate the committed notices for Qt and MoltenVK."""
    for name, text in (("qt", qt_notices()), ("moltenvk", moltenvk_notices())):
        target = ROOT / "third_party" / name / "NOTICES.txt"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)
        print(f"Wrote {target.relative_to(ROOT)}", flush=True)


def macho_files(bundle):
    """Every Mach-O file in the bundle, symlinks skipped."""
    for path in sorted(bundle.rglob("*")):
        if path.is_symlink() or not path.is_file():
            continue
        with path.open("rb") as stream:
            if stream.read(4) in MACHO_MAGIC:
                yield path


def capture(command):
    return subprocess.run(
        [str(part) for part in command], capture_output=True, text=True, check=True
    ).stdout


def signing_identity():
    """The one Developer ID Application identity, by its certificate hash."""
    if configured := os.environ.get("LAPIS_SIGN_IDENTITY"):
        return configured
    found = re.findall(
        r"\d+\) ([0-9A-F]{40}) \"Developer ID Application: [^\"]+\"",
        capture(["security", "find-identity", "-v", "-p", "codesigning"]),
    )
    if len(set(found)) != 1:
        raise PackageError(
            "Need exactly one Developer ID Application identity in the keychain "
            f"(found {len(set(found))}); choose one with LAPIS_SIGN_IDENTITY"
        )
    return found[0]


def fetch_sparkle():
    """Sparkle's pinned release, unpacked once; returns its framework."""
    framework = SPARKLE / "Sparkle.framework"
    archive = DOWNLOADS / f"Sparkle-{SPARKLE_VERSION}.tar.xz"
    if not archive.exists() or sha256(archive) != SPARKLE_SHA256:
        DOWNLOADS.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(SPARKLE_URL) as response:
            archive.write_bytes(response.read())
        if sha256(archive) != SPARKLE_SHA256:
            archive.unlink()
            raise PackageError("Sparkle's archive does not match its pinned SHA-256")
        shutil.rmtree(SPARKLE, ignore_errors=True)
    if not framework.exists():
        SPARKLE.mkdir(parents=True, exist_ok=True)
        run(["tar", "-xJf", archive, "-C", SPARKLE])
    return framework


def build_lapis():
    if not (QT_PREFIX / f".qtdeclarative-{QT_VERSION}").exists():
        raise PackageError("Build Qt first: package_macos.py qt")
    sparkle = fetch_sparkle()
    try:
        ghostty = ghostty_prefix()
    except SetupError as error:
        raise PackageError(str(error)) from error
    run(
        ["cmake", "-S", ROOT, "-B", APP_BUILD]
        + common_cmake_arguments()
        + [
            f"-DCMAKE_PREFIX_PATH={QT_PREFIX}",
            "-DLAPIS_BUILD_DESKTOP=ON",
            "-DLAPIS_PACKAGE=ON",
            "-DBUILD_TESTING=OFF",
            f"-DLAPIS_GHOSTTY_PREFIX={ghostty}",
            f"-DVulkan_INCLUDE_DIR={VULKAN_INCLUDE}",
            f"-DVulkan_LIBRARY={MOLTENVK_LIBRARY}",
            f"-DLAPIS_SPARKLE_FRAMEWORK={sparkle}",
        ]
    )
    run(
        ["cmake", "--build", APP_BUILD, "--target", "lapis_desktop"]
        + ["--parallel", str(JOBS)]
    )
    return APP_BUILD / "apps" / "desktop" / "lapis.app"


# Highway (inside Ghostty's library) names its header in assertion messages by
# the Zig cache path it was built in, which holds the builder's home folder.
ZIG_CACHE_PATH = re.compile(
    rb"(?<=\0)/[\x21-\x7e]*?/zig-local-cache/o/[0-9a-f]+/([\x21-\x7e]*?)\0"
)


def scrub_build_paths(path):
    """Keep only the header's own name, at the same length, in an executable."""
    data = path.read_bytes()

    def shorten(match):
        kept = match.group(1) + b"\0"
        return kept + b"\0" * (len(match.group(0)) - len(kept))

    scrubbed = ZIG_CACHE_PATH.sub(shorten, data)
    if scrubbed != data:
        path.write_bytes(scrubbed)


def deploy(built):
    """Copy the build into a bundle that carries Qt, MoltenVK and the notices."""
    shutil.rmtree(APP.parent, ignore_errors=True)
    APP.parent.mkdir(parents=True)
    run(["ditto", built, APP])
    contents = APP / "Contents"
    for executable in (contents / "MacOS").iterdir():
        scrub_build_paths(executable)
    extras = []
    for plugin in EXTRA_PLUGINS:
        target = contents / "PlugIns" / plugin
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(QT_PREFIX / "plugins" / plugin, target)
        extras.append(target)
    run(
        [QT_PREFIX / "bin" / "macdeployqt", APP]
        + [f"-qmldir={ROOT / 'apps/desktop/qml'}", "-always-overwrite"]
        + [f"-executable={path}" for path in extras]
        + [f"-executable={contents / 'MacOS' / 'lapis_session_service'}"]
    )
    for unused in UNUSED_PLUGINS:
        shutil.rmtree(contents / "PlugIns" / unused, ignore_errors=True)
    # Sparkle, without the XPC services only a sandboxed app needs, and arm64
    # like everything else.
    sparkle = contents / "Frameworks" / "Sparkle.framework"
    shutil.rmtree(sparkle, ignore_errors=True)
    run(["ditto", SPARKLE / "Sparkle.framework", sparkle])
    shutil.rmtree(sparkle / "Versions" / "B" / "XPCServices", ignore_errors=True)
    for path in macho_files(sparkle):
        if len(capture(["lipo", "-archs", path]).split()) > 1:
            run(["lipo", path, "-thin", ARCHITECTURE, "-output", path])
    moltenvk = contents / "Frameworks" / "libMoltenVK.dylib"
    shutil.copy2(MOLTENVK_LIBRARY, moltenvk)
    moltenvk.chmod(0o755)
    run(["install_name_tool", "-id", "@rpath/libMoltenVK.dylib", moltenvk])
    notices = contents / "Resources" / "Notices"
    notices.mkdir(exist_ok=True)
    for name, source in NOTICES.items():
        shutil.copy2(source, notices / name)
    # Build-machine rpaths go; the bundle's own Frameworks folder stays.
    for path in macho_files(APP):
        for rpath in re.findall(
            r"cmd LC_RPATH\n.*\n\s+path (\S+)", capture(["otool", "-l", path])
        ):
            if rpath.startswith("/"):
                run(["install_name_tool", "-delete_rpath", rpath, path])
        run(["strip", "-S", "-x", path], stderr=subprocess.DEVNULL)


def sign(identity):
    """Sign inside out with the hardened runtime and a secure timestamp."""
    base = ["codesign", "--force", "--timestamp", "--options", "runtime"]
    contents = APP / "Contents"
    frameworks = sorted((contents / "Frameworks").glob("*.framework"))
    loose = [
        path
        for path in macho_files(APP)
        if not any(framework in path.parents for framework in frameworks)
        and path.parent != contents / "MacOS"
    ]
    for path in loose:
        run(base + ["--sign", identity, path])
    for framework in frameworks:
        # Code nested in a framework (Sparkle's updater) is signed before it.
        nested = sorted(framework.rglob("*.app"))
        for bundle in nested:
            run(base + ["--sign", identity, bundle])
        for path in macho_files(framework):
            if path.name != framework.stem and not any(
                b in path.parents for b in nested
            ):
                run(base + ["--sign", identity, path])
        run(base + ["--sign", identity, framework])
    run(
        base
        + ["--identifier", "dev.lapis.session-service", "--sign", identity]
        + [contents / "MacOS" / "lapis_session_service"]
    )
    run(base + ["--entitlements", ENTITLEMENTS, "--sign", identity, APP])
    run(["codesign", "--verify", "--deep", "--strict", "--verbose=2", APP])


def command_app(_arguments):
    deploy(build_lapis())
    sign(signing_identity())
    print(f"Signed {APP.relative_to(ROOT)}", flush=True)


def notarize(path, profile):
    """Submit, wait, and staple Apple's ticket to the file."""
    upload = path
    if path.suffix == ".app":
        APP_ZIP.unlink(missing_ok=True)
        run(["ditto", "-c", "-k", "--keepParent", path, APP_ZIP])
        upload = APP_ZIP
    result = json.loads(
        capture(
            ["xcrun", "notarytool", "submit", upload, "--keychain-profile", profile]
            + ["--wait", "--output-format", "json"]
        )
    )
    print(f"Notarization {result.get('id')}: {result.get('status')}", flush=True)
    if result.get("status") != "Accepted":
        run(
            [
                "xcrun",
                "notarytool",
                "log",
                result.get("id"),
                "--keychain-profile",
                profile,
            ]
        )
        raise PackageError(f"Apple did not accept {path.name}")
    run(["xcrun", "stapler", "staple", path])


def command_dmg(_arguments):
    if not APP.exists():
        raise PackageError("Build the app first: package_macos.py app")
    folder = RELEASE / "dmg"
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    run(["ditto", APP, folder / "lapis.app"])
    (folder / "Applications").symlink_to("/Applications")
    DMG.unlink(missing_ok=True)
    run(
        ["hdiutil", "create", "-volname", "lapis", "-srcfolder", folder]
        + ["-fs", "HFS+", "-format", "UDZO", "-imagekey", "zlib-level=9", DMG]
    )
    run(["codesign", "--force", "--timestamp", "--sign", signing_identity(), DMG])
    shutil.rmtree(folder)
    print(f"{DMG.relative_to(ROOT)}  sha256 {sha256(DMG)}", flush=True)


def command_notarize(arguments):
    notarize(APP, arguments.profile)
    command_dmg(arguments)
    notarize(DMG, arguments.profile)
    print(f"{DMG.relative_to(ROOT)}  sha256 {sha256(DMG)}", flush=True)


def check_binaries(problems):
    """arm64, macOS 14 or earlier, and nothing linked from outside the bundle."""
    for path in macho_files(APP):
        name = path.relative_to(APP)
        if capture(["lipo", "-archs", path]).split() != [ARCHITECTURE]:
            problems.append(f"{name} is not {ARCHITECTURE} only")
        for minimum in re.findall(r"minos (\S+)", capture(["otool", "-l", path])):
            if tuple(map(int, minimum.split("."))) > tuple(
                map(int, MACOS_TARGET.split("."))
            ):
                problems.append(f"{name} needs macOS {minimum}")
        for line in capture(["otool", "-L", path]).splitlines()[1:]:
            library = line.strip().split(" (")[0]
            if library and not library.startswith(ALLOWED_LIBRARY_PREFIXES):
                problems.append(f"{name} links {library}")


def check_identifying_strings(problems):
    """No user name, host name or build path of this Mac inside the bundle."""
    host = socket.gethostname()
    terms = {f"/Users/{getpass.getuser()}", "/opt/homebrew", host, host.split(".")[0]}
    terms.update(filter(None, os.environ.get("LAPIS_SWEEP_TERMS", "").split(",")))
    needles = {term.encode() for term in terms if len(term) >= 4}
    for path in sorted(APP.rglob("*")):
        if path.is_symlink() or not path.is_file():
            continue
        data = path.read_bytes()
        for needle in needles:
            if needle.lower() in data.lower():
                problems.append(
                    f"{path.relative_to(APP)} contains an identifying string"
                )
                break


def check_moltenvk(problems):
    """The bundled MoltenVK answers Vulkan the way Qt asks it, with no loader."""
    library = ctypes.CDLL(str(APP / "Contents/Frameworks/libMoltenVK.dylib"))
    handle = ctypes.c_void_p

    class ApplicationInfo(ctypes.Structure):
        _fields_ = [
            ("sType", ctypes.c_int),
            ("pNext", handle),
            ("pApplicationName", ctypes.c_char_p),
            ("applicationVersion", ctypes.c_uint32),
            ("pEngineName", ctypes.c_char_p),
            ("engineVersion", ctypes.c_uint32),
            ("apiVersion", ctypes.c_uint32),
        ]

    class InstanceInfo(ctypes.Structure):
        _fields_ = [
            ("sType", ctypes.c_int),
            ("pNext", handle),
            ("flags", ctypes.c_uint32),
            ("pApplicationInfo", ctypes.POINTER(ApplicationInfo)),
            ("enabledLayerCount", ctypes.c_uint32),
            ("ppEnabledLayerNames", handle),
            ("enabledExtensionCount", ctypes.c_uint32),
            ("ppEnabledExtensionNames", ctypes.POINTER(ctypes.c_char_p)),
        ]

    extensions = (ctypes.c_char_p * 2)(b"VK_KHR_surface", b"VK_EXT_metal_surface")
    application = ApplicationInfo(
        0, None, b"lapis", 1, b"lapis", 1, (1 << 22) | (2 << 12)
    )
    info = InstanceInfo(1, None, 0, ctypes.pointer(application), 0, None, 2, extensions)
    instance = handle()
    library.vkCreateInstance.restype = ctypes.c_int
    if library.vkCreateInstance(ctypes.byref(info), None, ctypes.byref(instance)) != 0:
        problems.append(
            "MoltenVK could not create a Vulkan instance with a Metal surface"
        )
        return
    count = ctypes.c_uint32()
    library.vkEnumeratePhysicalDevices(instance, ctypes.byref(count), None)
    if count.value == 0:
        problems.append("MoltenVK found no GPU")
    library.vkDestroyInstance(instance, None)
    print(f"MoltenVK: {count.value} GPU(s) through VK_EXT_metal_surface", flush=True)


def check_headless_host(problems):
    """The app's windowless host starts, answers a request and stops cleanly."""
    home = Path(tempfile.mkdtemp(prefix="lapis-check-", dir="/tmp"))
    try:
        host = subprocess.Popen(
            [APP / "Contents/MacOS/lapis", "--serve"],
            env={**os.environ, "LAPIS_HOME": str(home)},
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        control = home / "runtime" / "workspace-control.sock"
        deadline = time.monotonic() + 20
        while (
            not control.exists() and time.monotonic() < deadline and host.poll() is None
        ):
            time.sleep(0.2)
        answer = b""
        if control.exists():
            with socket.socket(socket.AF_UNIX) as client:
                client.settimeout(10)
                client.connect(str(control))
                client.sendall(b'{"version":1,"request":"harnesses"}\n')
                while not answer.endswith(b"\n"):
                    chunk = client.recv(65536)
                    if not chunk:
                        break
                    answer += chunk
        if b'"harnesses"' not in answer:
            problems.append("The headless host did not answer a harnesses request")
        else:
            print(f"Headless host answered {len(answer)} bytes", flush=True)
        host.send_signal(signal.SIGTERM)
        try:
            output = host.communicate(timeout=10)[0].decode(errors="replace")
        except subprocess.TimeoutExpired:
            host.kill()
            output = host.communicate()[0].decode(errors="replace")
            problems.append("The headless host did not stop on SIGTERM")
        if problems:
            print(output[-4000:], flush=True)
    finally:
        shutil.rmtree(home, ignore_errors=True)


def check_launchd_start(problems):
    """Started by launchd, as from the Dock, the app takes the login shell's PATH."""
    shell = os.environ.get("SHELL", "/bin/zsh")
    bare = {
        "HOME": str(Path.home()),
        "USER": getpass.getuser(),
        "SHELL": shell,
        "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
    }
    expected = subprocess.run(
        [shell, "-i", "-l", "-c", 'printf "\\n%s" "$PATH"'],
        env=bare,
        stdin=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        text=True,
        timeout=20,
    ).stdout.splitlines()[-1]
    home = Path(tempfile.mkdtemp(prefix="lapis-launchd-", dir="/tmp"))
    log = home / "lapis.log"
    label = "dev.lapis.release-check"
    try:
        run(
            ["launchctl", "submit", "-l", label, "-o", log, "-e", log, "--"]
            + ["/usr/bin/env", f"LAPIS_HOME={home}", "QT_FORCE_STDERR_LOGGING=1"]
            + [APP / "Contents/MacOS/lapis", "--serve"]
        )
        control = home / "runtime" / "workspace-control.sock"
        deadline = time.monotonic() + 30
        while not control.exists() and time.monotonic() < deadline:
            time.sleep(0.2)
        output = log.read_text(errors="replace") if log.exists() else ""
        if not control.exists():
            problems.append("The app did not start under launchd")
        elif f"and PATH {expected}" not in output:
            problems.append(
                "Started by launchd, the app did not take the login shell's PATH"
            )
        else:
            print("Under launchd, the app took the login shell's PATH", flush=True)
    finally:
        subprocess.run(["launchctl", "remove", label], check=False)
        time.sleep(1)
        shutil.rmtree(home, ignore_errors=True)


def check_updates(problems):
    """The app knows where its updates come from and whose key signs them."""
    info = plistlib.loads((APP / "Contents" / "Info.plist").read_bytes())
    key = (ROOT / "apps/desktop/macos/update-public-key.txt").read_text().strip()
    if info.get("SUPublicEDKey") != key or not str(
        info.get("SUFeedURL", "")
    ).startswith(RELEASES):
        problems.append("the app has no update feed or key")
    if not (
        APP / "Contents/Frameworks/Sparkle.framework/Versions/B/Autoupdate"
    ).exists():
        problems.append("Sparkle's updater is missing")


def command_verify(arguments):
    problems = []
    check_updates(problems)
    run(["codesign", "--verify", "--deep", "--strict", "--verbose=2", APP])
    check_binaries(problems)
    check_identifying_strings(problems)
    check_moltenvk(problems)
    check_headless_host(problems)
    check_launchd_start(problems)
    if arguments.notarized:
        run(["spctl", "--assess", "--type", "execute", "--verbose=4", APP])
        run(["xcrun", "stapler", "validate", APP])
        run(["xcrun", "stapler", "validate", DMG])
        run(
            [
                "spctl",
                "--assess",
                "--type",
                "open",
                "--context",
                "context:primary-signature",
            ]
            + ["--verbose=4", DMG]
        )
    if problems:
        raise PackageError("\n  ".join(["the app is not ready:"] + problems))
    print("The app passed every check", flush=True)


def command_release(arguments):
    """Attach the notarized DMG and the Qt sources to a GitHub release."""
    run(["xcrun", "stapler", "validate", DMG])
    commit = capture(["git", "-C", ROOT, "rev-parse", "HEAD"]).strip()
    if capture(["git", "-C", ROOT, "branch", "-r", "--contains", commit]).strip() == "":
        raise PackageError("Push the commit the app was built from first")
    version = arguments.tag.removeprefix("v")
    notes = "\n".join(
        [
            f"lapis {version} for Apple silicon Macs with macOS 14 or later.",
            "",
            "Open lapis-macos-arm64.dmg and drag lapis to Applications.",
            f"SHA-256: {sha256(DMG)}",
            "",
            "The Qt archives are the exact sources of the Qt frameworks inside the",
            "app, which it uses under the LGPL-3.0. Notices are in",
            "lapis.app/Contents/Resources/Notices.",
        ]
    )
    sources = [
        DOWNLOADS / f"{name}-everywhere-src-{QT_VERSION}.tar.xz" for name in QT_MODULES
    ]
    write_appcast(arguments.tag, version)
    run(
        ["gh", "release", "create", arguments.tag, "--target", commit]
        + ["--title", f"lapis {version}", "--notes", notes]
        + (["--draft"] if arguments.draft else [])
        + [DMG, APPCAST, *sources]
    )


def update_signature(path):
    """Sparkle's EdDSA signature for a file. The key stays in the login
    keychain; generate_keys (which made it) exports it for this one call to a
    private temporary file, since sign_update itself would need a prompt."""
    with tempfile.TemporaryDirectory() as folder:
        key = Path(folder) / "key"
        run([SPARKLE / "bin" / "generate_keys", "-x", key], stdout=subprocess.DEVNULL)
        output = capture([SPARKLE / "bin" / "sign_update", "--ed-key-file", key, path])
    found = re.search(r'sparkle:edSignature="([^"]+)" length="(\d+)"', output)
    if not found:
        raise PackageError("sign_update did not sign the DMG")
    return found.group(1), found.group(2)


def write_appcast(tag, version):
    """The update feed the app reads from the latest release's assets."""
    signature, length = update_signature(DMG)
    published = time.strftime("%a, %d %b %Y %H:%M:%S +0000", time.gmtime())
    APPCAST.write_text(
        f"""<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
  <channel>
    <title>lapis</title>
    <item>
      <title>lapis {version}</title>
      <pubDate>{published}</pubDate>
      <link>{RELEASES}/tag/{tag}</link>
      <sparkle:version>{version}</sparkle:version>
      <sparkle:shortVersionString>{version}</sparkle:shortVersionString>
      <sparkle:minimumSystemVersion>{MACOS_TARGET}</sparkle:minimumSystemVersion>
      <enclosure url="{RELEASES}/download/{tag}/{DMG.name}" type="application/octet-stream"
                 sparkle:edSignature="{signature}" length="{length}"/>
    </item>
  </channel>
</rss>
"""
    )


def command_all(arguments):
    command_qt(arguments)
    command_app(arguments)
    command_notarize(arguments)
    arguments.notarized = True
    command_verify(arguments)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("qt", help="Build Qt with Vulkan from pinned source")
    commands.add_parser("icon", help="Remake the macOS icon from the phone's")
    commands.add_parser("notices", help="Regenerate the Qt and MoltenVK notices")
    commands.add_parser("app", help="Build, bundle and sign lapis.app")
    commands.add_parser("dmg", help="Put the signed app in a signed DMG")
    for name, text in (
        ("notarize", "Notarize and staple the app, then the DMG"),
        ("all", "Every step, then verify"),
    ):
        step = commands.add_parser(name, help=text)
        step.add_argument(
            "--profile", required=True, help="notarytool keychain profile"
        )
    release = commands.add_parser("release", help="Publish a GitHub release")
    release.add_argument("--tag", required=True, help="for example v0.1.0")
    release.add_argument("--draft", action="store_true", help="Leave it as a draft")
    verify = commands.add_parser("verify", help="Check the bundle before release")
    verify.add_argument(
        "--notarized", action="store_true", help="Also check Gatekeeper"
    )
    arguments = parser.parse_args()
    handlers = {
        "qt": command_qt,
        "icon": command_icon,
        "notices": command_notices,
        "app": command_app,
        "dmg": command_dmg,
        "notarize": command_notarize,
        "verify": command_verify,
        "release": command_release,
        "all": command_all,
    }
    try:
        handlers[arguments.command](arguments)
    except (PackageError, subprocess.CalledProcessError) as error:
        print(f"package: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
