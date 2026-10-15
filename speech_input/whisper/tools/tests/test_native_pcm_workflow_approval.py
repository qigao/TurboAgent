"""Prevent a PR update from silently downloading licensed human audio."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[4]
WORKFLOW = ROOT / ".github/workflows/speech-whisper-offline-cli.yml"


class ExplicitRealAudioOptIn(unittest.TestCase):
    def test_real_audio_download_is_dispatch_only_and_not_an_artifact(self):
        data = WORKFLOW.read_text(encoding="utf-8")
        name = "      - name: Manual pinned public voice native ABI2 fake-vendor parity"
        end = "      - name: Archive pinned source and unmodified stdout/dependency evidence"
        self.assertIn(name, data)
        self.assertIn(end, data)
        prefix, tail = data.split(name, 1)
        manual, artifact = tail.split(end, 1)
        self.assertIn("github.event_name == 'workflow_dispatch'", manual)
        self.assertIn(
            "github.ref == 'refs/heads/test/speech-native-abi2-pcm-parity-146'",
            manual)
        self.assertIn("curl --fail", manual)
        self.assertNotIn("curl --fail", prefix)
        self.assertIn("rm -rf --", manual)
        self.assertNotIn("PINNED_HUMAN_SOURCE", artifact)
        self.assertNotIn("speech-c11-real-pcm-", artifact)
        self.assertNotIn("$private", artifact)

    def test_synthetic_probes_run_on_pr_and_use_fake_model_only(self):
        data = WORKFLOW.read_text(encoding="utf-8")
        early = data.split(
            "      - name: Manual pinned public voice native ABI2 fake-vendor parity",
            1)[0]
        self.assertIn("Compare real C11 ABI2 PCM/M1/M2/Provider with immutable generated boundaries",
                      early)
        self.assertIn("verify_native_guard_pcm.py", early)
        self.assertIn("lag1_35 lag1_36 lag1_37", early)
        self.assertIn('fake-model.bin', early)
        self.assertNotIn("ggml-tiny.bin", early)

    def test_yaml_step_titles_are_not_unquoted_colon_scalars(self):
        # GitHub rejects an unquoted step name with a colon and a space
        # even before running any tests. Keep a narrow registration gate.
        data = WORKFLOW.read_text(encoding="utf-8")
        for line in data.splitlines():
            if re.match(r"^\s*- name: ", line):
                value = line.split("- name: ", 1)[1]
                with self.subTest(step=value):
                    self.assertFalse(": " in value and not value.startswith(
                        ("'", '"')), "YAML plain-scalar colon needs quoting")


if __name__ == "__main__":
    unittest.main()
