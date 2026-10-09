import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("notes", Path(__file__).with_name("notes.py"))
notes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(notes)


class NotesTests(unittest.TestCase):
    def test_supported_subjects(self):
        for kind, _ in notes.GROUPS:
            for scope in ("", "(sema)"):
                for breaking in ("", "!"):
                    with self.subTest(kind=kind, scope=scope, breaking=breaking):
                        match = notes.SUBJECT_RE.fullmatch(f"{kind}{scope}{breaking}: message")
                        self.assertIsNotNone(match)
                        self.assertEqual(match.group("kind"), kind)
                        self.assertEqual(bool(match.group("breaking")), bool(breaking))

    def test_breaking_changes_first(self):
        subjects = "feat!: break one\nfix(sema)!: break two\nfix(sema): repair\nfeat: feature\n"
        with patch.object(notes, "previous_tag", return_value="v0.5.1"), patch.object(notes, "git", return_value=subjects), patch("sys.argv", ["notes.py", "--version", "0.5.2"]), patch("builtins.print") as output:
            notes.main()
        text = output.call_args.args[0]
        self.assertLess(text.index("## Breaking changes"), text.index("## Features"))
        self.assertIn("- **sema:** break two", text)
        self.assertIn("## Fixes\n\n- **sema:** repair", text)
        self.assertEqual(text.count("break one"), 1)
        self.assertIn("Changes since v0.5.1 (4 commits).", text)

    def test_repository_subjects(self):
        subjects = notes.git("log", "--format=%s", "v0.5.1..HEAD").splitlines()
        self.assertTrue(subjects)
        for subject in subjects:
            if subject.startswith(tuple(kind + "(" for kind, _ in notes.GROUPS)):
                self.assertIsNotNone(notes.SUBJECT_RE.fullmatch(subject), subject)


if __name__ == "__main__":
    unittest.main()
