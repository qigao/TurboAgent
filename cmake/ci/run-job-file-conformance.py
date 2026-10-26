#!/usr/bin/env python3
"""Run and audit the native Job file gate; never infer success from an empty set."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET

# Independent qualification oracle. Changing CMake's registrations must not
# silently shrink the required case set or process-stage coverage.
CUTS = (
    'delivery_intent_before', 'delivery_intent_after', 'delivery_intent_lost_ack',
    'delivery_intent_returned', 'delivery_after_send', 'delivery_receipt_before',
    'delivery_receipt_after', 'delivery_receipt_lost_ack', 'delivery_receipt_returned',
)
EXPECTED = {'test_turbo_agent_job_file_verifier', *(f'test_job_files_{c}' for c in CUTS)}
FILTER = r'^(test_turbo_agent_job_file_verifier|test_job_files_delivery_.*)$'
SOURCE = Path(__file__).resolve().parents[2]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def check_inventory(data):
    require(isinstance(data, dict), 'Invalid CTest inventory object')
    tests = data.get('tests', [])
    require(isinstance(tests, list) and all(isinstance(t, dict) for t in tests), 'Invalid CTest test list')
    names = [t.get('name') for t in tests]
    require(all(isinstance(n, str) for n in names), 'Invalid CTest names')
    require(len(names) == len(EXPECTED) and set(names) == EXPECTED, f'Wrong Job file inventory: {names}')
    for t in tests:
        require(bool(t.get('command')), f'Missing test executable: {t["name"]}')
        props = {p.get('name'): p.get('value') for p in t.get('properties', []) if isinstance(p, dict)}
        require(not props.get('DISABLED', False), f'Disabled test: {t["name"]}')
    return sorted(names)


def check_junit(path):
    root = ET.parse(path).getroot()
    require(root.tag == 'testsuite', 'Unexpected CTest JUnit root')
    require(int(root.get('tests', '-1')) == len(EXPECTED), 'Wrong JUnit test count')
    for key in ('failures', 'errors', 'skipped', 'disabled'):
        require(int(root.get(key, '0')) == 0, f'JUnit {key} is not zero')
    tests = root.findall('testcase')
    names = [t.get('name') for t in tests]
    require(len(names) == len(EXPECTED) and set(names) == EXPECTED, 'Missing/duplicate JUnit cases')
    for t in tests:
        require(t.get('status') in (None, 'run'), f'Non-run JUnit case: {t.get("name")}')
        require(not any(t.find(tag) is not None for tag in ('failure', 'error', 'skipped')), 'JUnit failure/skip child')
    return len(tests)


def check_traces(directory):
    require({p.name for p in directory.glob('*.tsv')} == {f'{c}.tsv' for c in CUTS}, 'Missing/extra cut traces')
    processes = 0
    for cut in CUTS:
        crash = '87' if cut.endswith('lost_ack') else '86'
        expected = [('init', '0'), ('crash', crash), ('inspect', '0'), ('denied', '0'), ('stale', '0'),
                    ('verification_source_changed', '0'), ('verification_evidence_missing', '0'),
                    ('verification_evidence_changed', '0'), ('recover', '0'), ('replay', '0')]
        with (directory / f'{cut}.tsv').open(newline='', encoding='utf-8') as stream:
            rows = list(csv.reader(stream, delimiter='\t'))
        require(rows == [[stage, code, code] for stage, code in expected], f'Wrong stage/exit trace: {cut}: {rows}')
        processes += len(rows)
    return processes


def read_cache(path):
    entries = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        match = re.fullmatch(r'([^#/:][^:]*):[^=]+=(.*)', line)
        if match:
            entries[match[1]] = match[2]
    return entries


def cache_boolean(cache, key):
    value = cache.get(key, '').upper()
    if value in ('1', 'ON', 'YES', 'TRUE', 'Y'):
        return True
    if value in ('0', 'OFF', 'NO', 'FALSE', 'N'):
        return False
    raise ValueError(f'Expected explicit CMake boolean for {key}: {value!r}')


def check_host(cache, require_mac):
    require(cache_boolean(cache, 'TURBOAGENT_JOB_FILE_CONFORMANCE'), 'Explicit Job file conformance must be ON')
    if require_mac:
        require(platform.system() == 'Darwin' and platform.machine() == 'arm64', 'Requires actual macOS arm64 host')
        require(cache.get('CMAKE_OSX_ARCHITECTURES') == 'arm64', 'Requires arm64 build')
        require(not cache_boolean(cache, 'BUILD_TESTS'), 'macOS dedicated profile must keep full tests OFF')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--evidence-dir', required=True, type=Path)
    parser.add_argument('--ctest', default=shutil.which('ctest'))
    parser.add_argument('--preset')
    parser.add_argument('--expected-commit')
    parser.add_argument('--require-macos-arm64', action='store_true')
    args = parser.parse_args(argv)
    build, evidence = args.build_dir.resolve(), args.evidence_dir.resolve()
    require(not evidence.exists(), f'Refusing to reuse qualification evidence: {evidence}')
    evidence.mkdir(parents=True)
    report = {'passed': False, 'host_os': platform.system(), 'host_arch': platform.machine(), 'build_dir': str(build),
              'require_macos_arm64': args.require_macos_arm64, 'preset': args.preset}
    try:
        commit = subprocess.check_output(['git', '-C', str(SOURCE), 'rev-parse', 'HEAD'], text=True).strip()
        dirty = bool(subprocess.check_output(['git', '-C', str(SOURCE), 'status', '--porcelain', '--untracked-files=no'], text=True).strip())
        report.update(checkout_sha=commit, tracked_source_dirty=dirty)
        if args.expected_commit:
            require(commit == args.expected_commit and not dirty, 'Checkout differs from expected clean CI commit')
        cache_path = build / 'CMakeCache.txt'
        cache = read_cache(cache_path)
        report['cache'] = {k: cache.get(k) for k in ('BUILD_TESTS', 'TURBOAGENT_JOB_FILE_CONFORMANCE',
                                                   'TURBOAGENT_SQLITE_VFS_CONFORMANCE', 'CMAKE_OSX_ARCHITECTURES')}
        report['cache_sha256'] = hashlib.sha256(cache_path.read_bytes()).hexdigest()
        check_host(cache, args.require_macos_arm64)
        require(args.ctest is not None, 'CTest executable is missing')
        prefix = [args.ctest, '--test-dir', str(build)]
        if args.preset:
            prefix += ['--preset', args.preset]
        command = prefix + ['--show-only=json-v1', '-R', FILTER]
        inventory = subprocess.run(command, cwd=SOURCE, text=True, capture_output=True, timeout=30)
        (evidence / 'inventory.json').write_text(inventory.stdout, encoding='utf-8')
        (evidence / 'inventory-stderr.log').write_text(inventory.stderr, encoding='utf-8')
        require(inventory.returncode == 0, 'CTest inventory failed')
        require(bool(inventory.stdout.strip()), 'CTest returned an empty inventory')
        report['tests'] = check_inventory(json.loads(inventory.stdout))
        cuts = evidence / 'cuts'
        cuts.mkdir()
        env = os.environ.copy()
        env['TURBOAGENT_JOB_FILE_EVIDENCE_DIR'] = str(cuts)
        command = prefix + ['-R', FILTER, '--no-tests=error', '--verbose', '-j', '2',
                            '--output-junit', str(evidence / 'junit.xml')]
        report['ctest_command'] = command
        with (evidence / 'ctest.log').open('w', encoding='utf-8') as output:
            completed = subprocess.run(command, cwd=SOURCE, env=env, stdout=output, stderr=subprocess.STDOUT, timeout=600)
        report['ctest_exit'] = completed.returncode
        require(completed.returncode == 0, 'CTest failed; inspect ctest.log and retained cases')
        report['passed_tests'] = check_junit(evidence / 'junit.xml')
        report['fixture_application_processes'] = check_traces(cuts)
        report['passed'] = True
    except (ValueError, OSError, subprocess.SubprocessError, ET.ParseError) as error:
        report['error'] = str(error)
    finally:
        report['evidence_sha256'] = {str(p.relative_to(evidence)): hashlib.sha256(p.read_bytes()).hexdigest()
                                     for p in sorted(evidence.rglob('*')) if p.is_file()}
        (evidence / 'qualification.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    summary = {k: report[k] for k in ('passed', 'host_os', 'host_arch', 'checkout_sha',
                                     'passed_tests', 'fixture_application_processes', 'error') if k in report}
    print(json.dumps({**summary, 'evidence_dir': str(evidence)}))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
