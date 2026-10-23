import importlib.util
from pathlib import Path
import tempfile
import unittest
import zipfile


SPEC = importlib.util.spec_from_file_location("meta_sd", Path(__file__).resolve().parents[1] / "meta_sd.py")
mod = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(mod)


class TransferTests(unittest.TestCase):
    def test_zip_into_root_and_subdirectory(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            archive = root / "payload.zip"
            with zipfile.ZipFile(archive, "w") as z:
                z.writestr("A/file.txt", "bonjour")
            result = mod.unzip(root, archive, ".")
            self.assertEqual(result["files"], 1)
            self.assertEqual((root / "A/file.txt").read_text(), "bonjour")
            mod.unzip(root, archive, "games/new")
            self.assertEqual((root / "games/new/A/file.txt").read_text(), "bonjour")

    def test_zip_escape_refused_before_writes(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            archive = root / "bad.zip"
            with zipfile.ZipFile(archive, "w") as z:
                z.writestr("good.txt", "a")
                z.writestr("../outside.txt", "b")
            with self.assertRaises(ValueError):
                mod.unzip(root, archive, ".")
            self.assertFalse((root / "good.txt").exists())

    def test_sd_symlink_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "link").symlink_to("/tmp")
            with self.assertRaises(ValueError):
                mod.within(root, "link/file")


if __name__ == "__main__":
    unittest.main()
