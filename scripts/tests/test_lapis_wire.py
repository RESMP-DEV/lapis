"""The shared Python wire module against the C++ protocol and launch identity.

The expected values are read from the C++ sources, so a change on either side
fails here instead of drifting silently.
"""

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "apps" / "remote"))
import lapis_wire as wire  # noqa: E402

from scripts import check_cli_launch  # noqa: E402

LAUNCH_TEST = ROOT / "services/session/tests/launch_spec_test.cpp"
PROTOCOL = ROOT / "services/session/src/transport/local_protocol.hpp"


def cpp_enum(source, name):
    """The enumerator names of `enum class <name>`, comments removed."""
    body = re.search(rf"enum class {name}\b[^{{]*\{{(.*?)\}};", source, re.S)
    assert body, f"enum class {name} not found"
    text = re.sub(r"//[^\n]*", "", body.group(1))
    entries = [entry.strip() for entry in text.split(",") if entry.strip()]
    values, next_value = {}, 0
    for entry in entries:
        identifier, _, value = (part.strip() for part in entry.partition("="))
        next_value = int(value) if value else next_value
        values[identifier] = next_value
        next_value += 1
    return values


def cpp_constant(source, name):
    match = re.search(rf"constexpr \w+ {name} = ([^;]+);", source)
    assert match, f"{name} not found"
    value = re.sub(r"qsizetype\{(\d+)\}", r"\1", match.group(1))
    assert re.fullmatch(r"[\d *]+", value), value
    product = 1
    for factor in value.split("*"):
        product *= int(factor)
    return product


class FingerprintTests(unittest.TestCase):
    # The launch launch_spec_test.cpp pins: /bin/sh resume abc in /.
    PROGRAM, ARGUMENTS, DIRECTORY = "/bin/sh", ["resume", "abc"], "/"

    def pinned(self):
        source = LAUNCH_TEST.read_text(encoding="utf-8")
        block = source[source.index("const LaunchSpec pinned{") :]
        self.assertIn('.program = QStringLiteral("/bin/sh")', block)
        self.assertIn('{QStringLiteral("resume"), QStringLiteral("abc")}', block)
        self.assertIn('.directory = QStringLiteral("/")', block)
        found = dict(re.findall(r'AgentMode::(\w+),\s*"([0-9a-f]{64})"', block[:2000]))
        self.assertEqual(set(found), {"terminal", "codex", "claude"})
        return found

    def test_matches_the_cpp_pinned_fingerprints(self):
        for mode, expected in self.pinned().items():
            with self.subTest(mode=mode):
                digest = wire.fingerprint(
                    self.PROGRAM,
                    self.ARGUMENTS,
                    self.DIRECTORY,
                    "" if mode == "terminal" else mode,
                )
                self.assertEqual(digest.hex(), expected)

    def test_check_scripts_compute_the_same_fingerprint(self):
        pinned = self.pinned()
        for flags, mode in (({}, "terminal"), ({"codex": True}, "codex")):
            with self.subTest(mode=mode):
                digest = check_cli_launch.fingerprint(
                    self.PROGRAM, self.ARGUMENTS, self.DIRECTORY, **flags
                )
                self.assertEqual(digest.hex(), pinned[mode])
        digest = check_cli_launch.fingerprint(
            self.PROGRAM, self.ARGUMENTS, self.DIRECTORY, claude=True
        )
        self.assertEqual(digest.hex(), pinned["claude"])

    def test_hashes_the_values_as_given(self):
        # Like launch_fingerprint, the shared function does not canonicalize:
        # an uncleaned path is a different launch.
        self.assertNotEqual(
            wire.fingerprint("/bin//sh", self.ARGUMENTS, self.DIRECTORY),
            wire.fingerprint(self.PROGRAM, self.ARGUMENTS, self.DIRECTORY),
        )
        self.assertEqual(
            wire.fingerprint(self.PROGRAM, self.ARGUMENTS, Path("/")),
            wire.fingerprint(self.PROGRAM, self.ARGUMENTS, "/"),
        )


class ProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = PROTOCOL.read_text(encoding="utf-8")

    def test_message_kinds_match_the_cpp_enum(self):
        kinds = cpp_enum(self.source, "Kind")
        self.assertEqual(kinds["hello"], 1)
        for name, value in kinds.items():
            with self.subTest(kind=name):
                self.assertEqual(getattr(wire, name.upper()), value)

    def test_attach_modes_and_statuses_match_the_cpp_enums(self):
        for name, value in cpp_enum(self.source, "AttachMode").items():
            with self.subTest(mode=name):
                self.assertEqual(getattr(wire, name.upper()), value)
        self.assertEqual(
            cpp_enum(self.source, "StatusCode"),
            {name: code for code, name in wire.STATUS_NAMES.items()},
        )

    def test_limits_match_the_cpp_constants(self):
        for name, value in (
            ("version", wire.WIRE_VERSION),
            ("max_frame_bytes", wire.MAX_FRAME),
            ("max_cells", wire.MAX_CELLS),
            ("max_codepoints", wire.MAX_CODEPOINTS),
            ("max_paste_bytes", wire.MAX_PASTE),
            ("snapshot_header_bytes", wire.SNAPSHOT_HEADER),
        ):
            with self.subTest(constant=name):
                self.assertEqual(cpp_constant(self.source, name), value)
        self.assertEqual(check_cli_launch.VERSION, wire.WIRE_VERSION)

    def test_qt_string_is_big_endian_utf16_with_a_byte_length(self):
        self.assertEqual(wire.qt_string(""), bytes(4))
        self.assertEqual(wire.qt_string("é"), b"\0\0\0\2\0\xe9")
        # Outside the BMP: a surrogate pair, four bytes.
        self.assertEqual(wire.qt_string("\U0001f600"), b"\0\0\0\4\xd8\x3d\xde\x00")
        self.assertEqual(wire.qt_string(Path("/a")), wire.qt_string("/a"))

    def test_frame_counts_the_kind_byte_in_its_length(self):
        self.assertEqual(wire.frame(wire.READY), b"\0\0\0\1\x09")
        self.assertEqual(wire.frame(wire.TEXT, b"ab"), b"\0\0\0\3\x03ab")

    def test_status_message_names_the_code(self):
        self.assertEqual(wire.status_message(b"\x01no"), "rejected: no")
        self.assertEqual(wire.status_message(b"\x04"), "overloaded:")
        self.assertEqual(wire.status_message(b"\x09x"), "status: x")
        self.assertEqual(wire.status_message(b""), "status:")
        self.assertEqual(wire.status_message(b"\x02\xff"), "ended: �")


if __name__ == "__main__":
    unittest.main()
