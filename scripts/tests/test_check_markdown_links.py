import tempfile
import unittest
from pathlib import Path

from scripts import check_markdown_links


class MarkdownLinkCheckerTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def write(self, relative: str, text: str) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def test_accepts_local_paths_fragments_and_external_links(self):
        self.write(
            "docs/a.md",
            "# Alpha\n\n[alpha](#alpha)\n[beta](b.md#beta)\n[web](https://example.test)\n",
        )
        self.write("docs/b.md", "# Beta\n")
        self.assertEqual(check_markdown_links.collect_failures(self.root), [])

    def test_reports_missing_path_and_fragment(self):
        self.write("docs/a.md", "[missing](missing.md)\n[bad](b.md#missing)\n")
        self.write("docs/b.md", "# Beta\n")
        failures = check_markdown_links.collect_failures(self.root)
        self.assertEqual(len(failures), 2)
        self.assertTrue(any("missing target" in failure for failure in failures))
        self.assertTrue(any("missing fragment" in failure for failure in failures))

    def test_distinguishes_duplicate_heading_anchors(self):
        self.write("a.md", "# Same\n# Same\n[first](#same)\n[second](#same-1)\n")
        self.assertEqual(check_markdown_links.collect_failures(self.root), [])

    def test_ignores_generated_and_private_trees(self):
        self.write("build/generated.md", "[broken](missing.md)\n")
        self.write("runtime/private.md", "[broken](missing.md)\n")
        self.assertEqual(check_markdown_links.collect_failures(self.root), [])


if __name__ == "__main__":
    unittest.main()
