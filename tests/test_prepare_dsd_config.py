import tempfile
import unittest
from pathlib import Path

from tools.prepare_dsd_config import ConfigError, adapt_config, prepare_config


class DsdConfigTests(unittest.TestCase):
    def test_adds_padding_values_from_extractor_metadata(self):
        source = "section_padding_value: 255\nfile_image_padding_value: 0\nheader: header.yaml\n"

        result = adapt_config(source)

        self.assertIn("  arm9: 255\n", result)
        self.assertIn("  file_image: 0\n", result)
        self.assertIn("  rom: 255\n", result)
        self.assertIn("header: header.yaml\n", result)

    def test_preserves_existing_dsd_padding_block(self):
        source = "padding:\n  arm9: 42\n"

        self.assertEqual(adapt_config(source), source)

    def test_rejects_missing_padding_fields(self):
        with self.assertRaisesRegex(ConfigError, "section_padding_value"):
            adapt_config("header: header.yaml\n")

    def test_writes_sidecar_and_preserves_extracted_config(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "config.yaml"
            destination = Path(directory) / "config-dsd.yaml"
            original = "section_padding_value: 255\nfile_image_padding_value: 255\n"
            source.write_text(original, encoding="utf-8")

            prepare_config(source, destination)

            self.assertEqual(source.read_text(encoding="utf-8"), original)
            self.assertIn("padding:\n", destination.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
