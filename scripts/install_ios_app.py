"""Build the lapis iPhone app for a device and install it with devicectl.

Signs with an installed Apple Development identity and a development
provisioning profile that covers dev.lapis.remote (a wildcard profile works)
and includes the phone. The app's default gateway is this Mac's Tailscale
name. Without --device, the one paired iPhone that devicectl can reach is used.

    uv run --no-project python scripts/install_ios_app.py [--device ID] [--build-only]
        [--wait MINUTES]
"""

import argparse
import hashlib
import json
import plistlib
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from check_ios_remote import APP_ID, APP_SOURCES, tool  # noqa: E402

PRODUCTS = ROOT / "build" / "ios" / "device"
PROFILE_DIRECTORIES = [
    Path.home() / "Library/Developer/Xcode/UserData/Provisioning Profiles",
    Path.home() / "Library/MobileDevice/Provisioning Profiles",
]


def decode_profile(path):
    return plistlib.loads(
        subprocess.run(
            ["security", "cms", "-D", "-i", str(path)],
            capture_output=True,
            check=True,
        ).stdout
    )


def matches(pattern, identifier):
    return pattern == identifier or (
        pattern.endswith("*") and identifier.startswith(pattern[:-1])
    )


def signing_identities():
    """SHA-1 of each valid code-signing certificate in the keychain."""
    output = tool("security", "find-identity", "-v", "-p", "codesigning")
    return {
        line.split()[1] for line in output.splitlines() if line.strip()[:1].isdigit()
    }


def choose_profile(identities):
    """The newest iOS development profile for this app with a usable certificate."""
    best = None
    for directory in PROFILE_DIRECTORIES:
        for path in directory.glob("*.mobileprovision"):
            profile = decode_profile(path)
            team = profile["TeamIdentifier"][0]
            pattern = profile["Entitlements"]["application-identifier"].split(".", 1)[1]
            if "iOS" not in profile.get("Platform", []) or not matches(pattern, APP_ID):
                continue
            if not profile["Entitlements"].get("get-task-allow"):
                continue
            certificates = {
                hashlib.sha1(certificate).hexdigest().upper()
                for certificate in profile.get("DeveloperCertificates", [])
            }
            usable = sorted(certificates & identities)
            if not usable:
                continue
            candidate = (profile["ExpirationDate"], path, profile, team, usable[0])
            if best is None or candidate[0] > best[0]:
                best = candidate
    if best is None:
        raise SystemExit(
            "No development provisioning profile covers dev.lapis.remote with an installed "
            "certificate. Open apps/ios in Xcode once (xcodegen, then build to the phone)."
        )
    return best[1:]


def mac_host():
    status = json.loads(tool("tailscale", "status", "--json"))
    return status["Self"]["DNSName"].rstrip(".")


def build(host):
    identities = signing_identities()
    path, profile, team, identity = choose_profile(identities)
    sdk = tool("xcrun", "--sdk", "iphoneos", "--show-sdk-path")
    app = PRODUCTS / "Lapis.app"
    shutil.rmtree(app, ignore_errors=True)
    app.mkdir(parents=True)
    tool(
        "xcrun",
        "swiftc",
        "-target",
        "arm64-apple-ios17.0",
        "-sdk",
        sdk,
        "-parse-as-library",
        "-O",
        "-module-name",
        "Lapis",
        *sorted(str(source) for source in APP_SOURCES.glob("*.swift")),
        "-o",
        str(app / "Lapis"),
    )
    partial = PRODUCTS / "assets.plist"
    tool(
        "xcrun",
        "actool",
        str(APP_SOURCES / "Assets.xcassets"),
        "--compile",
        str(app),
        "--platform",
        "iphoneos",
        "--minimum-deployment-target",
        "17.0",
        "--app-icon",
        "AppIcon",
        "--target-device",
        "iphone",
        "--output-partial-info-plist",
        str(partial),
    )
    values = {
        "DEVELOPMENT_LANGUAGE": "en",
        "EXECUTABLE_NAME": "Lapis",
        "PRODUCT_BUNDLE_IDENTIFIER": APP_ID,
        "PRODUCT_NAME": "Lapis",
        "LAPIS_DEFAULT_HOST": host,
    }
    info = plistlib.loads((APP_SOURCES / "Info.plist").read_bytes())
    for key, value in info.items():
        if isinstance(value, str) and value.startswith("$(") and value.endswith(")"):
            info[key] = values[value[2:-1]]
    info.update(plistlib.loads(partial.read_bytes()))
    info.update(
        {
            "CFBundleSupportedPlatforms": ["iPhoneOS"],
            "MinimumOSVersion": "17.0",
            "UIDeviceFamily": [1],
            "LSRequiresIPhoneOS": True,
            "DTPlatformName": "iphoneos",
            "UIRequiredDeviceCapabilities": ["arm64"],
        }
    )
    (app / "Info.plist").write_bytes(plistlib.dumps(info))
    shutil.copy2(path, app / "embedded.mobileprovision")
    with tempfile.NamedTemporaryFile("wb", suffix=".plist", delete=False) as handle:
        handle.write(
            plistlib.dumps(
                {
                    "application-identifier": f"{team}.{APP_ID}",
                    "com.apple.developer.team-identifier": team,
                    "get-task-allow": True,
                }
            )
        )
        entitlements = handle.name
    try:
        tool(
            "codesign",
            "--force",
            "--sign",
            identity,
            "--entitlements",
            entitlements,
            "--timestamp=none",
            "--generate-entitlement-der",
            str(app),
        )
    finally:
        Path(entitlements).unlink()
    tool("codesign", "--verify", "--strict", str(app))
    devices = len(profile.get("ProvisionedDevices", []))
    print(f"built {app} for {host} (profile {profile['Name']}, {devices} device)")
    return app


def reachable_phone():
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as handle:
        output = Path(handle.name)
    try:
        try:
            result = subprocess.run(
                ["xcrun", "devicectl", "list", "devices", "--json-output", str(output)],
                capture_output=True,
                text=True,
                timeout=60,
            )
        except subprocess.TimeoutExpired as error:
            raise SystemExit(
                "devicectl list devices timed out after 60 seconds"
            ) from error
        except OSError as error:
            raise SystemExit(f"Could not run devicectl: {error}") from error
        if result.returncode != 0:
            raise SystemExit(
                f"devicectl list devices failed:\n{result.stdout}{result.stderr}"
            )
        try:
            devices = json.loads(output.read_text())["result"]["devices"]
            if not isinstance(devices, list) or not all(
                isinstance(device, dict)
                and isinstance(device.get("identifier"), str)
                and isinstance(device.get("hardwareProperties", {}), dict)
                and isinstance(device.get("connectionProperties", {}), dict)
                for device in devices
            ):
                raise ValueError("Invalid device records")
        except (OSError, ValueError, KeyError, TypeError) as error:
            raise SystemExit(
                "devicectl did not return a readable device list:\n"
                f"{result.stdout}{result.stderr}"
            ) from error
    finally:
        output.unlink(missing_ok=True)
    phones = [
        device
        for device in devices
        if device.get("hardwareProperties", {}).get("deviceType") == "iPhone"
        and device.get("connectionProperties", {}).get("tunnelState") != "unavailable"
    ]
    if len(phones) > 1:
        names = ", ".join(phone.get("name") or phone["identifier"] for phone in phones)
        raise SystemExit(
            f"Multiple iPhones are reachable ({names}); pass --device with one of "
            "their identifiers."
        )
    return phones[0]["identifier"] if phones else None


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--device", help="devicectl identifier; defaults to the paired iPhone"
    )
    parser.add_argument(
        "--host", help="gateway host; defaults to this Mac's Tailscale name"
    )
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument(
        "--wait",
        type=float,
        default=0,
        help="minutes to wait for the phone to become reachable",
    )
    args = parser.parse_args()
    app = build(args.host or mac_host())
    if args.build_only:
        return 0
    deadline = time.monotonic() + args.wait * 60
    device = args.device or reachable_phone()
    while device is None and time.monotonic() < deadline:
        time.sleep(30)
        device = reachable_phone()
    if device is None:
        raise SystemExit(
            "No iPhone is reachable. Unlock it on the same Wi-Fi as this Mac (or connect "
            "a cable) and run this again."
        )
    subprocess.run(
        [
            "xcrun",
            "devicectl",
            "device",
            "install",
            "app",
            "--device",
            device,
            str(app),
        ],
        check=True,
    )
    print("installed; open lapis on the phone")
    return 0


if __name__ == "__main__":
    sys.exit(main())
