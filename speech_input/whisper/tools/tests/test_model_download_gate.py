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
        self.assertIn("github.run_attempt == 1", text)
        self.assertIn('test "${{ github.run_attempt }}" = 1', text)
        self.assertIn("github.event.label.name == 'speech-model-download-approved'", text)
        self.assertNotIn("types: [opened, reopened, synchronize, labeled]", text)
        self.assertNotIn("workflow_dispatch:", text)

    def test_exact_trusted_repository_and_branch(self):
        text = self.workflow
        for required in (
            "github.repository == 'qigao/TurboAgent'",
            "github.event.pull_request.head.repo.full_name == 'qigao/TurboAgent'",
            "github.event.pull_request.head.ref == 'test/speech-correlated-corpus-integration-146'",
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

    def test_generated_noise_diagnostics_preserve_approval_and_no_raw_text_upload(self):
        text = self.workflow
        for required in (
            "Generate original, license-independent zero and nonzero noise probes",
            "generate_noise_fixtures.py",
            "Observe real tiny-model response to generated non-speech",
            "evaluate_non_speech_probe.py",
            "/tmp/ta-real-asr-nonspeech-fixture-metadata.json",
            "/tmp/ta-real-asr-nonspeech-outcomes.json",
        ):
            with self.subTest(required=required):
                self.assertIn(required, text)
        self.assertLess(text.index("Download ONLY explicitly approved immutable MIT"),
                        text.index("Observe real tiny-model response"))
        upload = text.split("name: speech-whisper-real-model-gated-metrics-NO-WEIGHTS", 1)[1]
        for forbidden in (
            "/tmp/ta-real-asr-nonspeech-private",
            "no-speech-probes/",
            "stationary-noise-quiet.wav",
            "stationary-noise-medium.wav",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, upload)

    def test_opt_in_stationary_guard_real_model_is_approved_once_and_no_private_text(self):
        text = self.workflow
        self.assertIn("Qualify explicit guard on real JFK speech and exact stationary-noise probes", text)
        self.assertIn("--stationarity-guard 1", text)
        self.assertIn("/tmp/ta-real-asr-guard-metrics.json", text)
        self.assertIn("/tmp/ta-real-asr-guard-jfk-metrics.json", text)
        upload = text.split("name: speech-whisper-real-model-gated-metrics-NO-WEIGHTS", 1)[1]
        for forbidden in ("ta-real-asr-guard-jfk-private.txt",
                          "/tmp/ta-guard-digital-zero.stdout",
                          "/tmp/ta-guard-stationary-noise-quiet.stdout",
                          "/tmp/ta-guard-stationary-noise-medium.stdout"):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, upload)

    def test_agc_jfk_false_rejection_gate_uses_same_approved_model_only(self):
        text = self.workflow
        for piece in (
            "Generate trusted fixed-RMS spoken JFK stress probe without model access",
            "generate_agc_speech_probe.py",
            "jfk-agc-private.wav",
            "cmp -s /tmp/ta-real-asr-agc-guard-0.private.stdout",
            "/tmp/ta-real-asr-agc-guard-comparison.json",
            "guard_preserved_exact_baseline_text",
        ):
            with self.subTest(piece=piece):
                self.assertIn(piece, text)
        artifact=text.split(
            "name: speech-whisper-real-model-gated-metrics-NO-WEIGHTS",1)[1]
        self.assertNotIn("/tmp/ta-real-asr-agc-guard-0.private.stdout",artifact)
        self.assertNotIn("/tmp/ta-real-asr-agc-guard-1.private.stdout",artifact)
        self.assertNotIn("jfk-agc-private.wav",artifact)

    def test_soft_and_short_jfk_voice_comparison_keeps_private_transcripts_local(self):
        workflow = self.workflow
        for required in (
            "generate_speech_stress_probes.py",
            "Compare baseline against opt-in guard on genuine soft and short JFK voice derivatives",
            "compare_speech_stress_outcomes.py",
            "/tmp/ta-real-asr-soft-jfk-comparison.json",
            "/tmp/ta-real-asr-speech-stress-manifest.json",
            "jfk-mid-short-0p8s",
        ):
            with self.subTest(required=required):
                self.assertIn(required, workflow)
        upload = workflow.split(
            "name: speech-whisper-real-model-gated-metrics-NO-WEIGHTS", 1)[1]
        for private in (
            "/tmp/ta-real-asr-soft-jfk-private",
            "jfk-speech-stress-private/",
            "-guard0.stdout", "-guard1.stdout",
            "jfk-soft-8.wav", "jfk-agc-soft-32.wav",
        ):
            with self.subTest(private=private):
                self.assertNotIn(private, upload)

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
