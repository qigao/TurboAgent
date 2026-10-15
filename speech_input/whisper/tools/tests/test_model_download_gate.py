"""Ensure actual ASR weights cannot be fetched by an ordinary PR update.

These are static, dependency-free guardrails for the checked-in workflow.
They do not download model weights or assert genuine acoustic recognition.
"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[4]
WORKFLOW = ROOT / ".github/workflows/speech-whisper-real-model-gated.yml"

class MaintainerModelAdmission(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workflow = WORKFLOW.read_text(encoding="utf-8")

    def test_only_explicit_label_event_triggers_weights_job(self):
        text = self.workflow
        self.assertIn("types: [labeled]", text)
        self.assertIn("github.event.action == 'labeled'", text)
        self.assertIn("github.event.label.name == 'speech-model-download-approved'", text)
        self.assertNotIn("types: [opened, reopened, synchronize, labeled]", text)
        self.assertNotIn("workflow_dispatch:", text)

    def test_exact_trusted_repository_and_branch(self):
        text = self.workflow
        for required in (
            "github.repository == 'qigao/TurboAgent'",
            "github.event.pull_request.head.repo.full_name == 'qigao/TurboAgent'",
            "github.event.pull_request.head.ref == 'test/speech-whisper-real-model-gated-137'",
            "ref: ${{ github.event.pull_request.head.sha }}",
        ):
            with self.subTest(required=required):
                self.assertIn(required, text)

    def test_immutable_source_and_weight_bytes(self):
        text = self.workflow
        for required in (
            "d1be6fde11ac6e0407606b4e42fe72d34add8037",
            "6cf093dc053db98bb21b1788eb2fc1ada4df195e",
            "5359861c739e955e79d9a303bcbc70fb988958b1",
            "be07e048e1e599ad46341c8d2a135645097a538221678b7acdd1b1919c6e1b21",
            "MODEL_BYTES: '77691713'",
            "sha256sum --check --status",
            'test "$size" = "$MODEL_BYTES"',
        ):
            with self.subTest(required=required):
                self.assertIn(required, text)

    def test_artifacts_never_contain_weights(self):
        text = self.workflow
        self.assertIn("name: speech-whisper-real-model-gated-metrics-NO-WEIGHTS", text)
        self.assertIn("/tmp/ta-real-asr-acoustic-metrics.json", text)
        self.assertNotIn("${{ env.MODEL_ROOT }}", text)
        # A model hash is an audit receipt, not model bytes.
        upload_block = text.split("name: speech-whisper-real-model-gated-metrics-NO-WEIGHTS", 1)[1]
        self.assertNotIn("ggml-tiny.bin", upload_block)
        self.assertNotIn("jfk-canonical.wav", upload_block)

    def test_real_inference_is_not_replaced_by_fixture(self):
        text = self.workflow
        self.assertIn("Build upstream Whisper v1.9.5 CPU-only from exact SHA", text)
        self.assertIn("Build exact opt-in TurboAgent CLI and run actual model acoustic inference", text)
        self.assertIn("python3 speech_input/whisper/tools/evaluate_asr.py", text)
        self.assertIn("--language en --max-wer 0.65", text)
        self.assertNotIn("TA_SPEECH_TEST_ESCAPE", text)
        self.assertNotIn("mock_whisper_cli", text)

if __name__ == "__main__":
    unittest.main()
