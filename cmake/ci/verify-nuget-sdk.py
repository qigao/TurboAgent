"""Carry the producer's dependencies across the final NuGet packaging boundary."""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tarfile
import xml.etree.ElementTree as ET
import zipfile


SOURCE = Path(__file__).resolve().parents[2]
PROFILES = {"linux-x64": "ci-linux-release", "windows-x64": "ci-windows-release"}
ROOTS = ("SALTS_ROOT", "SALTS_UTILS_ROOT", "CHTTP_ROOT", "TURBOWASM_ROOT",
         "TURBOSCRIPT_ROOT", "PRAKTOR_ROOT")
RIDS = (*PROFILES, "macos-arm64", "android-arm64-v8a")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def inventory(root):
    return {p.relative_to(root).as_posix(): digest(p)
            for p in sorted(root.rglob("*")) if p.is_file()}


def stage(rid):
    cache = SOURCE / "build" / PROFILES[rid] / "CMakeCache.txt"
    keys = {"VCPKG_INSTALLED_DIR", "VCPKG_TARGET_TRIPLET",
            "CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER"}
    selected = {}
    for line in cache.read_text(encoding="utf-8").splitlines():
        key = line.split(":", 1)[0]
        if key in keys:
            selected[key] = line.split("=", 1)[1]
    require(selected.keys() == keys, "Producer cache must identify compilers and installed ports")
    ports = Path(selected["VCPKG_INSTALLED_DIR"]) / selected["VCPKG_TARGET_TRIPLET"]
    sdk = SOURCE / "build" / "sdk" / rid
    manifest = (sdk / "turboagent-sdk-manifest.txt").read_text(encoding="utf-8")
    context = SOURCE / "build" / "nuget-consumer-context" / rid
    require(not context.exists(), "Consumer context must have a fresh destination")
    context.mkdir(parents=True)
    shutil.copytree(ports, context / "ports")
    roots = []
    for name in ROOTS:
        value = os.environ.get(name)
        if value:
            shutil.copytree(value, context / name)
            roots.append(name)
    require(all(name in roots for name in ROOTS[:4]), "Required released dependency SDKs are missing")
    metadata = {"rid": rid, "commit": os.environ["GITHUB_SHA"], "cache": selected,
                "roots": roots, "sdk_manifest": manifest, "sdk_files": inventory(sdk)}
    (context / "producer.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    # Tar preserves Unix executable modes, which upload-artifact alone does not.
    archive = context.parent / (rid + ".tar.gz")
    with tarfile.open(archive, "w:gz", dereference=True) as output:
        output.add(context, arcname=rid)
    print(f"Staged {rid} consumer dependencies: triplet={selected['VCPKG_TARGET_TRIPLET']}; "
          f"SDK files={len(metadata['sdk_files'])}; context bytes={archive.stat().st_size}")


def safe_member(name):
    path = PurePosixPath(name)
    require(not path.is_absolute() and ".." not in path.parts and
            "\\" not in name and ":" not in name, f"Unsafe archive member: {name}")


def consume(rid, package_dir, context_dir):
    require(not (SOURCE / "build" / PROFILES[rid]).exists() and
            not (SOURCE / "build" / "sdk").exists(),
            "Final NuGet consumer must run without producer build/install trees")
    packages = list(package_dir.glob("*.nupkg"))
    require(len(packages) == 1, "Require exactly one final NuGet package")
    package = packages[0]
    destination = SOURCE / "build" / "nuget-consumer-package"
    dependencies = SOURCE / "build" / "nuget-consumer-dependencies"
    require(not destination.exists() and not dependencies.exists(), "Require fresh extraction destinations")
    with tarfile.open(context_dir / (rid + ".tar.gz")) as archive:
        for member in archive.getmembers():
            safe_member(member.name)
            require(member.isfile() or member.isdir(), "Consumer context must contain regular files/directories")
        archive.extractall(dependencies, filter="data")
    context = dependencies / rid
    metadata = json.loads((context / "producer.json").read_text(encoding="utf-8"))
    require(metadata["rid"] == rid and metadata["commit"] == os.environ["GITHUB_SHA"],
            "Consumer context RID/commit differs from this workflow checkout")
    with zipfile.ZipFile(package) as archive:
        names = archive.namelist()
        require(len(names) == len(set(names)), "Duplicate NuGet archive entries")
        for name in names:
            safe_member(name)
        spec = ET.fromstring(archive.read("TurboAgent.Native.nuspec"))
        ns = {"n": "http://schemas.microsoft.com/packaging/2011/10/nuspec.xsd"}
        identity = spec.find("n:metadata", ns)
        require(identity.findtext("n:id", namespaces=ns) == "TurboAgent.Native" and
                identity.findtext("n:version", namespaces=ns) == os.environ["PACKAGE_VERSION"] and
                identity.find("n:repository", ns).get("commit") == os.environ["GITHUB_SHA"],
                "Final NuGet identity/version/commit differs from this workflow")
        for target in RIDS:
            prefix = f"sdk/{target}/"
            require(prefix + "lib/cmake/TurboAgent/TurboAgentConfig.cmake" in names,
                    f"Missing {target} package config")
            manifest = archive.read(prefix + "turboagent-sdk-manifest.txt").decode("utf-8")
            fields = dict(line.split("=", 1) for line in manifest.splitlines() if not line.startswith("repo="))
            require(fields["rid"] == target and fields["commit"] == os.environ["GITHUB_SHA"] and
                    fields["version"] == os.environ["SDK_VERSION"], f"Wrong {target} SDK manifest")
        archive.extractall(destination)
    sdk = destination / "sdk" / rid
    require((sdk / "turboagent-sdk-manifest.txt").read_text(encoding="utf-8") == metadata["sdk_manifest"],
            "Packaged SDK dependency provenance differs from the producer")
    require(inventory(sdk) == metadata["sdk_files"],
            "Final NuGet SDK files/bytes differ from the qualified producer install")
    require(set(metadata["roots"]).issubset(ROOTS) and
            all(name in metadata["roots"] for name in ROOTS[:4]), "Invalid dependency SDK roots")
    env = os.environ.copy()
    env.update({name: (context / name).as_posix() if name in metadata["roots"] else "" for name in ROOTS})
    env.update(SDK_RID=rid, TURBOAGENT_CONSUMER_SDK_ROOT=sdk.as_posix(),
               TURBOAGENT_CONSUMER_PORTS_ROOT=(context / "ports").as_posix(),
               TURBOAGENT_CONSUMER_CC="cl" if os.name == "nt" else "gcc",
               TURBOAGENT_CONSUMER_CXX="cl" if os.name == "nt" else "g++")
    print(f"Final NuGet {package.name} sha256={digest(package)}; RID={rid}; "
          f"{len(metadata['sdk_files'])} installed SDK files match exactly; "
          f"producer triplet={metadata['cache']['VCPKG_TARGET_TRIPLET']}", flush=True)
    print("Producer build/install trees are absent; dependencies use relocated test context", flush=True)
    subprocess.run(["cmake", "-P", str(SOURCE / "cmake/ci/run-sdk-consumers.cmake")],
                   env=env, check=True, timeout=600)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("stage", "consume"))
    parser.add_argument("--rid", choices=PROFILES, required=True)
    parser.add_argument("--package-dir", type=Path)
    parser.add_argument("--context-dir", type=Path)
    args = parser.parse_args()
    if args.action == "stage":
        stage(args.rid)
    else:
        require(args.package_dir and args.context_dir, "Consumer requires package and context artifact directories")
        consume(args.rid, args.package_dir, args.context_dir)
