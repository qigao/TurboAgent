import copy
import importlib.util
from pathlib import Path
import tempfile
import unittest
import sys
from unittest.mock import patch
import xml.etree.ElementTree as ET

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('job_gate', Path(__file__).resolve().parents[1] / 'run-job-file-conformance.py')
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


def inventory():
    return {'tests': [{'name': n, 'command': ['test-program'], 'properties': []} for n in sorted(gate.EXPECTED)]}


def junit(path):
    root = ET.Element('testsuite', tests='10', failures='0', skipped='0')
    for name in sorted(gate.EXPECTED):
        ET.SubElement(root, 'testcase', name=name, status='run')
    ET.ElementTree(root).write(path)
    return root


def traces(path):
    for cut in gate.CUTS:
        crash = '87' if cut.endswith('lost_ack') else '86'
        stages = [('init', '0'), ('crash', crash), ('inspect', '0'), ('denied', '0'), ('stale', '0'),
                  ('verification_source_changed', '0'), ('verification_evidence_missing', '0'),
                  ('verification_evidence_changed', '0'), ('recover', '0'), ('replay', '0')]
        (path / f'{cut}.tsv').write_text(''.join(f'{stage}\t{code}\t{code}\n' for stage, code in stages))


class QualificationGuards(unittest.TestCase):
    def test_inventory_requires_exact_complete_enabled_set(self):
        original = inventory()
        self.assertEqual(len(gate.check_inventory(original)), 10)
        for mutation in ('empty', 'missing', 'extra', 'duplicate', 'disabled', 'no_command'):
            data = copy.deepcopy(original)
            if mutation == 'empty': data['tests'] = []
            if mutation == 'missing': data['tests'].pop()
            if mutation == 'extra': data['tests'].append({'name': 'test_job_files_extra', 'command': ['test-program']})
            if mutation == 'duplicate': data['tests'][-1] = data['tests'][0]
            if mutation == 'disabled': data['tests'][0]['properties'] = [{'name': 'DISABLED', 'value': True}]
            if mutation == 'no_command': del data['tests'][0]['command']
            with self.subTest(mutation=mutation), self.assertRaises(ValueError): gate.check_inventory(data)

    def test_junit_refuses_skipped_failed_disabled_and_duplicated_cases(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'junit.xml'
            junit(path)
            self.assertEqual(gate.check_junit(path), 10)
            for mutation in ('skipped', 'failure', 'error', 'notrun', 'duplicate', 'count', 'disabled'):
                root = junit(path)
                case = root.find('testcase')
                if mutation in ('skipped', 'failure', 'error'): ET.SubElement(case, mutation)
                if mutation == 'notrun': case.set('status', 'notrun')
                if mutation == 'duplicate': root.findall('testcase')[-1].set('name', case.get('name'))
                if mutation == 'count': root.set('tests', '9')
                if mutation == 'disabled': root.set('disabled', '1')
                ET.ElementTree(root).write(path)
                with self.subTest(mutation=mutation), self.assertRaises(ValueError): gate.check_junit(path)

    def test_each_cut_requires_ten_ordered_actual_exit_records(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            traces(path)
            self.assertEqual(gate.check_traces(path), 90)
            target = path / f'{gate.CUTS[0]}.tsv'
            for mutation in ('missing_cut', 'missing_stage', 'wrong_actual_exit', 'wrong_expected_exit', 'wrong_order'):
                traces(path)
                rows = target.read_text().splitlines()
                if mutation == 'missing_cut': target.unlink()
                if mutation == 'missing_stage': target.write_text('\n'.join(rows[:-1]) + '\n')
                if mutation == 'wrong_actual_exit': target.write_text('\n'.join([rows[0], 'crash\t0\t86', *rows[2:]]) + '\n')
                if mutation == 'wrong_expected_exit': target.write_text('\n'.join([rows[0], 'crash\t86\t0', *rows[2:]]) + '\n')
                if mutation == 'wrong_order': target.write_text('\n'.join(reversed(rows)) + '\n')
                with self.subTest(mutation=mutation), self.assertRaises(ValueError): gate.check_traces(path)

    def test_macos_requires_actual_arm64_native_dedicated_profile(self):
        cache = {'TURBOAGENT_JOB_FILE_CONFORMANCE': 'ON', 'BUILD_TESTS': 'OFF', 'CMAKE_OSX_ARCHITECTURES': 'arm64'}
        with patch.object(gate.platform, 'system', return_value='Darwin'), patch.object(gate.platform, 'machine', return_value='arm64'):
            gate.check_host(cache, True)
            for field, value in (('BUILD_TESTS', 'ON'), ('CMAKE_OSX_ARCHITECTURES', 'x86_64'), ('TURBOAGENT_JOB_FILE_CONFORMANCE', 'OFF')):
                with self.subTest(field=field), self.assertRaises(ValueError): gate.check_host({**cache, field: value}, True)
        for system, machine in (('Linux', 'arm64'), ('Darwin', 'x86_64')):
            with patch.object(gate.platform, 'system', return_value=system), patch.object(gate.platform, 'machine', return_value=machine):
                with self.subTest(system=system, machine=machine), self.assertRaises(ValueError): gate.check_host(cache, True)


if __name__ == '__main__':
    unittest.main()
