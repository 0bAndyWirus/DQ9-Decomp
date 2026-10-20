"""Offline checks for repository Markdown paths and GitHub-style heading anchors."""
from collections import Counter
import html
from pathlib import Path
import re
import tempfile
import unittest
from urllib.parse import unquote, urlsplit


ROOT = Path(__file__).resolve().parents[1]


def prose(text):
    """Ignore fenced examples, whose bracket syntax is not Markdown navigation."""
    return re.sub(r"^ {0,3}(`{3,}|~{3,})[^\n]*\n.*?^ {0,3}\1[^\n]*$", "", text,
                  flags=re.MULTILINE | re.DOTALL)


def anchors(text):
    result = set(re.findall(r'<(?:a|[\w]+)\b[^>]*\b(?:id|name)=[\"\']([^\"\']+)', text))
    counts = Counter()
    for match in re.finditer(r"^ {0,3}#{1,6}\s+(.+?)\s*#*\s*$", prose(text), re.MULTILINE):
        heading = re.sub(r"!?\[([^\]]+)\]\([^)]*\)", r"\1", match[1])
        heading = html.unescape(re.sub(r"<[^>]*>", "", heading)).lower()
        slug = re.sub(r"[^\w\- ]", "", heading).replace(" ", "-")
        count = counts[slug]
        counts[slug] += 1
        result.add(f"{slug}-{count}" if count else slug)
    return result


def destinations(text):
    text = prose(text)
    # Inline code is not a navigation link (e.g. command syntax in a table).
    text = re.sub(r"(`+).*?\1", "", text)
    refs = {}
    for match in re.finditer(r"^ {0,3}\[([^\]]+)\]:\s*(<[^>]+>|\S+)", text, re.MULTILINE):
        refs[match[1].casefold()] = match[2].strip("<>")
        yield match[2].strip("<>")
    for match in re.finditer(r"!?\[[^\]\n]*\]\(\s*(<[^>]+>|[^\s()]*(?:\([^)]*\)[^\s()]*)*)", text):
        yield match[1].strip("<>")
    for match in re.finditer(r"!?\[([^\]\n]+)\]\[([^\]\n]*)\]", text):
        key = (match[2] or match[1]).casefold()
        if key not in refs:
            yield "__missing_reference__/" + key


def broken_links(root, documents):
    errors = []
    for source in documents:
        for destination in destinations(source.read_text(encoding="utf-8")):
            parsed = urlsplit(html.unescape(destination))
            if parsed.scheme or parsed.netloc:
                continue
            target = source.parent / unquote(parsed.path) if parsed.path else source
            if not target.exists():
                errors.append(f"{source.relative_to(root)}: missing path {destination}")
            elif parsed.fragment and target.suffix.lower() == ".md":
                if unquote(parsed.fragment) not in anchors(target.read_text(encoding="utf-8")):
                    errors.append(f"{source.relative_to(root)}: missing anchor {destination}")
    return errors


class DocumentationLinksTests(unittest.TestCase):
    def test_repository_links(self):
        ignored = {".git", ".venv", "build", "extracted", "reference", "__pycache__"}
        documents = [p for p in ROOT.rglob("*.md")
                     if not ignored.intersection(p.relative_to(ROOT).parts)]
        self.assertEqual([], broken_links(ROOT, documents))

    def test_detects_missing_path_and_anchor(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            doc = root / "doc.md"
            doc.write_text("# Present\n[bad](absent.md)\n[bad](#absent)\n[ok](#present)\n")
            errors = broken_links(root, [doc])
            self.assertEqual(2, len(errors))
            self.assertIn("missing path", errors[0])
            self.assertIn("missing anchor", errors[1])

    def test_heading_and_link_syntax(self):
        text = "# A `flag` / value!\n# Repeated\n# Repeated\n```md\n# Not a heading\n[x](missing)\n```\n"
        self.assertEqual({"a-flag--value", "repeated", "repeated-1"}, anchors(text))
        self.assertEqual([], list(destinations(text)))
        self.assertEqual(["some%20file.md#heading", "https://example.org/a_(b)"],
                         list(destinations('[one](<some%20file.md#heading>) [two](https://example.org/a_(b))')))

    def test_reference_links_and_html_anchors(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            target = root / "space file.md"
            target.write_text('<a id="explicit"></a>\n# Heading\n')
            doc = root / "doc.md"
            doc.write_text('[valid][destination]\n[missing][undefined]\n'
                           '[destination]: <space%20file.md#explicit>\n'
                           '[wrong](space%20file.md#absent)\n')
            errors = broken_links(root, [doc])
            self.assertEqual(2, len(errors))
            self.assertTrue(any("undefined" in error for error in errors))
            self.assertTrue(any("missing anchor" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
