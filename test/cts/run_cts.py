#!/usr/bin/env python3
"""Run the OpenXR conformance test suite (CTS) against the WiVRn server.

The tests are grouped in tiers (see suites.toml and docs/testing.md):

  ci       GitHub-hosted runner: software Vulkan (Mesa lavapipe), virtual headset
  gpu      a host with a GPU, hardware video encoders, virtual headset
  headset  a real headset connected with ADB, running the client built from this tree

Each tier is a list of runs. A run starts an isolated server (its own runtime and
configuration directories, D-Bus session and port, so it does not disturb an installed
WiVRn), connects a headset, runs conformance_cli with one graphics plugin and a test
selection, then compares the results with the expected failures in suites.toml.

Commands:
  build-cts   clone and build the CTS version pinned in suites.toml
  extensions  list the extensions the runtime advertises and their test coverage
  run         run a tier
"""

from __future__ import annotations

import argparse
import ctypes
import datetime
import fnmatch
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path

import tomllib

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
SUITES = HERE / "suites.toml"

# Signs of a crashed server in its log
CRASH_PATTERNS = [
    r"stack smashing detected",
    r"Segmentation fault",
    r"Assertion .* failed",
    r"terminate called",
    r"exited, exit status [1-9]",
    r"killed by signal",
    r"AddressSanitizer",
]


def log(msg: str) -> None:
    print(f"[run_cts {time.strftime('%H:%M:%S')}] {msg}", flush=True)


def load_suites(path: Path) -> dict:
    with open(path, "rb") as f:
        return tomllib.load(f)


# ---------------------------------------------------------------------------
# Building the CTS


def build_cts(args, suites) -> int:
    dest = Path(args.dest).resolve()
    src = dest / "src"
    build = dest / "build"
    tag = suites["cts"]["tag"]
    if not src.exists():
        if not shutil.which("git-lfs"):
            log("git-lfs is required to fetch the CTS assets")
            return 1
        subprocess.run(
            ["git", "clone", "--depth", "1", "--branch", tag, suites["cts"]["repository"], src],
            check=True,
        )
    subprocess.run(["git", "-C", src, "lfs", "install", "--local"], check=True)
    subprocess.run(["git", "-C", src, "lfs", "pull"], check=True)
    subprocess.run(
        [
            "cmake",
            "-S",
            src,
            "-B",
            build,
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DBUILD_API_LAYERS=OFF",
            "-DBUILD_SDK_TESTS=OFF",
            *args.cmake_arg,
        ],
        check=True,
    )
    subprocess.run(["cmake", "--build", build, "--target", "conformance_cli"], check=True)
    log(f"conformance_cli: {cts_binary(build)}")
    return 0


def cts_binary(path: Path) -> Path:
    """Accept the CTS build directory, the conformance_cli directory or the binary."""
    path = Path(path)
    for candidate in (
        path,
        path / "conformance_cli",
        path / "src/conformance/conformance_cli/conformance_cli",
    ):
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate.resolve()
    raise SystemExit(f"conformance_cli not found in {path}, build it with: {sys.argv[0]} build-cts")


# ---------------------------------------------------------------------------
# Extensions advertised by the runtime


class XrExtensionProperties(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_int32),
        ("next", ctypes.c_void_p),
        ("extensionName", ctypes.c_char * 128),
        ("extensionVersion", ctypes.c_uint32),
    ]


XR_TYPE_EXTENSION_PROPERTIES = 2


def runtime_extensions(runtime_json: Path, loader: Path | None) -> dict[str, int]:
    """Enumerate the instance extensions of the runtime, in a child process so that the
    loader picks up XR_RUNTIME_JSON."""
    code = f"""
import ctypes, json, sys
sys.path.insert(0, {str(HERE)!r})
from run_cts import XrExtensionProperties, XR_TYPE_EXTENSION_PROPERTIES
lib = ctypes.CDLL({str(loader) if loader else "libopenxr_loader.so.1"!r})
count = ctypes.c_uint32(0)
res = lib.xrEnumerateInstanceExtensionProperties(None, 0, ctypes.byref(count), None)
if res != 0:
    sys.exit(f"xrEnumerateInstanceExtensionProperties failed: {{res}}")
props = (XrExtensionProperties * count.value)()
for p in props:
    p.type = XR_TYPE_EXTENSION_PROPERTIES
res = lib.xrEnumerateInstanceExtensionProperties(None, count, ctypes.byref(count), props)
if res != 0:
    sys.exit(f"xrEnumerateInstanceExtensionProperties failed: {{res}}")
print(json.dumps({{p.extensionName.decode(): p.extensionVersion for p in props}}))
"""
    env = dict(os.environ, XR_RUNTIME_JSON=str(runtime_json))
    out = subprocess.run(
        [sys.executable, "-I", "-c", code], env=env, capture_output=True, text=True, check=False
    )
    if out.returncode != 0:
        raise SystemExit(f"cannot enumerate the runtime extensions: {out.stderr.strip()}")
    return json.loads(out.stdout)


def find_loader(cts: Path | None) -> Path | None:
    if cts:
        for f in sorted(cts.parent.glob("libopenxr_loader.so*")):
            return f
    return None


def coverage_table(extensions: dict[str, int], suites: dict, results=None) -> tuple[list, bool]:
    """One row per advertised extension: how it is tested and, with results, the outcome."""
    rows = []
    complete = True
    mapping = suites.get("extensions", {})
    for ext in sorted(extensions):
        entry = mapping.get(ext)
        if entry is None:
            complete = False
            rows.append((ext, "MISSING", "no entry in suites.toml [extensions]", ""))
            continue
        tests = entry.get("tests", [])
        how = ", ".join(tests) if tests else f"none: {entry.get('reason', '')}"
        outcome = ""
        if results is not None and tests:
            outcome = extension_outcome(tests, results)
        rows.append((ext, "tested" if tests else "untested", how, outcome))
    return rows, complete


def extension_outcome(tests: list[str], results: list[RunResult]) -> str:
    seen = []
    for r in results:
        for name, status in r.tests.items():
            if any(fnmatch.fnmatchcase(name, t) for t in tests):
                seen.append(status)
    if not seen:
        return "not run"
    for status in ("FAIL", "XFAIL", "PASS", "XPASS"):
        if status in seen:
            return status
    return "SKIP"


def cmd_extensions(args, suites) -> int:
    build = Path(args.build_dir).resolve()
    cts = cts_binary(Path(args.cts)) if args.cts else None
    exts = runtime_extensions(runtime_manifest(build), find_loader(cts))
    rows, complete = coverage_table(exts, suites)
    if args.json:
        print(json.dumps([dict(zip(("extension", "kind", "tests", "outcome"), r)) for r in rows]))
    else:
        for ext, kind, how, _ in rows:
            print(f"{ext:55} {kind:9} {how}")
        print(f"{len(exts)} extensions advertised")
    if not complete:
        log("some advertised extensions have no entry in suites.toml [extensions]")
    return 0 if complete else 1


# ---------------------------------------------------------------------------
# Server, headset and CTS processes


def runtime_manifest(build: Path) -> Path:
    for name in ("openxr_wivrn-dev.json", "openxr_wivrn.json"):
        if (build / name).exists():
            return build / name
    raise SystemExit(f"no OpenXR runtime manifest in {build}, is it a server build directory?")


def kill_group(proc: subprocess.Popen | None, timeout: float = 10) -> None:
    if proc is None or proc.poll() is not None:
        return
    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        proc.wait(timeout)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        proc.wait()


def wait_for(predicate, timeout: float, interval: float = 0.2) -> bool:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate():
            return True
        time.sleep(interval)
    return predicate()


class Environment:
    """An isolated server environment: runtime/config directories and a private D-Bus."""

    def __init__(self, out: Path):
        # sockaddr_un paths are limited to 108 bytes: keep the runtime directory short
        self.root = Path(tempfile.mkdtemp(prefix="wivrn-cts-", dir="/tmp"))
        self.runtime = self.root / "run"
        self.config = self.root / "config"
        self.runtime.mkdir(mode=0o700)
        self.config.mkdir()
        self.out = out
        self.dbus = None
        self.env = dict(os.environ)
        self.env.update(
            XDG_RUNTIME_DIR=str(self.runtime),
            XDG_CONFIG_HOME=str(self.config),
        )
        # The server must not see the user's session bus or OpenXR settings
        for var in ("XR_RUNTIME_JSON", "DBUS_SESSION_BUS_ADDRESS", "WAYLAND_DISPLAY"):
            self.env.pop(var, None)

    def start_dbus(self) -> None:
        self.dbus = subprocess.Popen(
            ["dbus-daemon", "--session", "--nofork", "--print-address=1"],
            stdout=subprocess.PIPE,
            text=True,
            start_new_session=True,
            env=self.env,
        )
        address = self.dbus.stdout.readline().strip()
        if not address:
            raise RuntimeError("dbus-daemon did not start")
        self.env["DBUS_SESSION_BUS_ADDRESS"] = address

    def close(self) -> None:
        kill_group(self.dbus)
        shutil.rmtree(self.root, ignore_errors=True)


@dataclass
class RunResult:
    name: str
    plugin: str
    profile: str
    tests: dict[str, str] = field(default_factory=dict)  # top-level test -> status
    details: dict[str, list[str]] = field(default_factory=dict)  # failure messages
    errors: list[str] = field(default_factory=list)  # harness/server problems
    headset: dict = field(default_factory=dict)
    duration: float = 0
    cts_exit: int | None = None

    def counts(self) -> dict[str, int]:
        res: dict[str, int] = {}
        for status in self.tests.values():
            res[status] = res.get(status, 0) + 1
        return res

    def ok(self, strict: bool) -> bool:
        bad = {"FAIL"} | ({"XPASS"} if strict else set())
        return not self.errors and not any(s in bad for s in self.tests.values())


class Headset:
    def start(self, env: Environment, port: int, run_dir: Path) -> None: ...
    def wait_connected(self, server_log: Path) -> bool: ...
    def stop(self) -> dict: ...


class VirtualHeadset(Headset):
    def __init__(self, binary: Path, args: list[str]):
        self.binary = binary
        self.args = args
        self.proc = None

    def start(self, env, port, run_dir):
        self.ready = run_dir / "headset.ready"
        self.stats = run_dir / "headset.json"
        self.log = open(run_dir / "headset.log", "w")  # noqa: SIM115 (closed in stop)
        cmd = [
            str(self.binary),
            "--port",
            str(port),
            "--ready-file",
            str(self.ready),
            "--stats-file",
            str(self.stats),
            "--stats-interval",
            "0.5",
            *self.args,
        ]
        (run_dir / "headset.cmd").write_text(" ".join(cmd) + "\n")
        self.proc = subprocess.Popen(
            cmd, stdout=self.log, stderr=subprocess.STDOUT, start_new_session=True, env=env.env
        )

    def wait_connected(self, server_log):
        return wait_for(lambda: self.ready.exists() or self.proc.poll() is not None, 30) and (
            self.ready.exists()
        )

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                kill_group(self.proc)
        self.log.close()
        try:
            return json.loads(self.stats.read_text())
        except (OSError, ValueError):
            return {}


def adb(*args, check=True, capture=True) -> str:
    out = subprocess.run(["adb", *args], capture_output=capture, text=True, timeout=60, check=False)
    if check and out.returncode != 0:
        raise RuntimeError(f"adb {' '.join(args)} failed: {out.stderr.strip()}")
    return out.stdout if capture else ""


class DeviceHeadset(Headset):
    """A real headset over ADB, running the client from the same source tree."""

    def __init__(self, package: str, allow_wifi: bool):
        self.package = package
        self.allow_wifi = allow_wifi
        self.logcat = None

    def start(self, env, port, run_dir):
        if adb("get-state", check=False).strip() != "device":
            raise RuntimeError("no headset connected with ADB")
        if not adb("shell", "pm", "path", self.package, check=False).strip():
            raise RuntimeError(f"{self.package} is not installed on the headset")
        headset_ip = adb("shell", "ip", "-4", "-br", "addr", "show", "wlan0").split()[2]
        headset_ip = headset_ip.split("/")[0]
        route = subprocess.run(
            ["ip", "route", "get", headset_ip], capture_output=True, text=True, check=True
        ).stdout
        iface = re.search(r" dev (\S+)", route).group(1)
        host_ip = re.search(r" src (\S+)", route).group(1)
        if iface.startswith("wl") and not self.allow_wifi:
            raise RuntimeError(
                f"the headset is reached through {iface} (WiFi), use Ethernet or --allow-wifi"
            )
        version = re.search(r"versionName=(\S+)", adb("shell", "dumpsys", "package", self.package))
        (run_dir / "device.json").write_text(
            json.dumps(
                {
                    "package": self.package,
                    "version": version.group(1) if version else None,
                    "headset_ip": headset_ip,
                    "host_ip": host_ip,
                    "interface": iface,
                }
            )
        )
        adb("shell", "am", "force-stop", self.package)
        adb("logcat", "-c", check=False)
        self.logcat_file = open(run_dir / "logcat.txt", "w")  # noqa: SIM115 (closed in stop)
        self.logcat = subprocess.Popen(
            ["adb", "logcat", "-v", "threadtime"],
            stdout=self.logcat_file,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        adb("shell", "input", "keyevent", "KEYCODE_WAKEUP")
        adb("shell", "am", "broadcast", "-a", "com.oculus.vrpowermanager.prox_close")
        adb(
            "shell",
            "am",
            "start",
            "-a",
            "android.intent.action.VIEW",
            "-d",
            f"wivrn://{host_ip}:{port}",
            self.package,
        )

    def wait_connected(self, server_log):
        def connected():
            text = server_log.read_text(errors="replace")
            if "Incompatible protocol version" in text:
                raise RuntimeError(
                    "the client on the headset speaks another protocol version: "
                    "install the client built from this tree"
                )
            return "Client connected" in text

        return wait_for(connected, 60, 0.5)

    def stop(self):
        # Whether the client survived the tests, before stopping it
        alive = bool(adb("shell", "pidof", self.package, check=False).strip())
        adb("shell", "am", "force-stop", self.package, check=False)
        adb(
            "shell",
            "am",
            "broadcast",
            "-a",
            "com.oculus.vrpowermanager.automation_disable",
            check=False,
        )
        adb("shell", "input", "keyevent", "KEYCODE_SLEEP", check=False)
        kill_group(self.logcat)
        crashes = []
        if self.logcat:
            self.logcat_file.close()
            # Fatal errors of the client only, the log has the whole device
            for line in Path(self.logcat_file.name).read_text(errors="replace").splitlines():
                if re.search(r"FATAL EXCEPTION|Fatal signal \d+", line) and (
                    self.package in line or "wivrn" in line.lower()
                ):
                    crashes.append(line.strip())
        return {"client_alive": alive, "client_crashes": crashes}


# ---------------------------------------------------------------------------
# Results


def parse_junit(path: Path) -> tuple[dict[str, str], dict[str, list[str]]]:
    """Aggregate the Catch2 JUnit report by top-level test case."""
    tests: dict[str, str] = {}
    details: dict[str, list[str]] = {}
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError):
        return tests, details
    for tc in root.iter("testcase"):
        full = tc.get("name", "")
        name = full.split("/", 1)[0]
        if tc.find("failure") is not None or tc.find("error") is not None:
            status = "FAIL"
            for kind in ("failure", "error"):
                for f in tc.findall(kind):
                    text = (f.get("message") or "") + "\n" + (f.text or "")
                    details.setdefault(name, []).append(f"{full}: {text.strip()[:2000]}")
        elif tc.find("skipped") is not None:
            status = "SKIP"
        else:
            status = "PASS"
        # Any failed section fails the test, a test with only skipped sections is skipped
        if status == "FAIL" or name not in tests or (tests[name] == "SKIP" and status == "PASS"):
            tests[name] = status
    return tests, details


def expected_failure(suites: dict, tier: str, run: str, test: str, details: list[str]):
    for xf in suites.get("expected_failures", []):
        if "tiers" in xf and tier not in xf["tiers"]:
            continue
        if "runs" in xf and run not in xf["runs"]:
            continue
        if not fnmatch.fnmatchcase(test, xf["test"]):
            continue
        if "message" in xf and details and not any(re.search(xf["message"], d) for d in details):
            continue
        return xf
    return None


def classify(result: RunResult, suites: dict, tier: str) -> None:
    for test, status in list(result.tests.items()):
        xf = expected_failure(suites, tier, result.name, test, result.details.get(test, []))
        if xf is None:
            continue
        if status == "FAIL":
            result.tests[test] = "XFAIL"
        elif status == "PASS" and not xf.get("flaky", False):
            result.tests[test] = "XPASS"


def scan_server_log(path: Path) -> list[str]:
    text = path.read_text(errors="replace") if path.exists() else ""
    found = []
    for pattern in CRASH_PATTERNS:
        for m in re.finditer(pattern, text):
            line = text[text.rfind("\n", 0, m.start()) + 1 : text.find("\n", m.end())]
            found.append(f"server log: {line.strip()}")
    return found


# ---------------------------------------------------------------------------
# Running a tier


def vulkan_driver(name: str) -> Path:
    """An ICD manifest path, or "lavapipe" for Mesa's software driver."""
    if name == "lavapipe":
        for directory in (
            "/usr/share/vulkan/icd.d",
            "/usr/local/share/vulkan/icd.d",
            "/etc/vulkan/icd.d",
        ):
            found = sorted(Path(directory).glob("lvp_icd*.json"))
            if found:
                return found[0]
        raise RuntimeError("Mesa lavapipe not found (lvp_icd*.json), install mesa-vulkan-drivers")
    path = Path(name).resolve()
    if not path.is_file():
        raise RuntimeError(f"Vulkan driver manifest {path} not found")
    return path


# The server gave up on the headset: the tests would wait for an OpenXR system that never comes
SESSION_FAILURES = [r"Error creating WiVRn session", r"xrt_instance_create_system failed"]


def run_problem(server, headset, server_log: Path) -> str | None:
    if server.poll() is not None:
        return f"the server exited during the tests (status {server.returncode})"
    if isinstance(headset, VirtualHeadset) and headset.proc.poll() is not None:
        return "the virtual headset disconnected during the tests, see headset.log"
    text = server_log.read_text(errors="replace")
    for pattern in SESSION_FAILURES:
        if m := re.search(pattern + ".*", text):
            return f"server log: {m.group(0).strip()}"
    return None


def run_one(args, suites, tier_name, tier, run, out: Path, cts: Path, build: Path) -> RunResult:
    profile_name = run.get("profile", tier.get("profile", "default"))
    profile = suites["profiles"][profile_name]
    plugin = run.get("plugin", tier.get("plugin", "vulkan2"))
    if run.get("runner") == "wivrn":
        plugin = "vulkan"
    result = RunResult(name=run["name"], plugin=plugin, profile=profile_name)
    run_dir = out / run["name"]
    run_dir.mkdir(parents=True, exist_ok=True)
    start = time.monotonic()

    port = args.port or run.get("port", tier.get("port", 19757))
    env = Environment(out)
    server = None
    headset: Headset | None = None
    server_log = run_dir / "server.log"
    try:
        if args.vk_driver:
            env.env["VK_DRIVER_FILES"] = str(vulkan_driver(args.vk_driver))
        env.start_dbus()

        config = dict(tier.get("server_config", {}))
        config.update(run.get("server_config", {}))
        config.setdefault("publish-service", "none")
        config["port"] = port
        (run_dir / "server.json").write_text(json.dumps(config, indent=2))

        server_cmd = [
            str(build / "server/wivrn-server"),
            "-f",
            str(run_dir / "server.json"),
            "--no-encrypt",
            "--no-publish-service",
            "--no-manage-active-runtime",
            "--no-instructions",
        ]
        with open(server_log, "w") as f:
            server = subprocess.Popen(
                server_cmd, stdout=f, stderr=subprocess.STDOUT, start_new_session=True, env=env.env
            )
        if wait_for(lambda: server.poll() is not None, 1):
            raise RuntimeError(f"the server exited at startup, see {server_log}")

        if tier.get("headset", "virtual") == "virtual":
            headset_args = list(profile.get("args", [])) + list(run.get("headset_args", []))
            headset = VirtualHeadset(build / "test/wivrn-virtual-headset", headset_args)
        else:
            headset = DeviceHeadset(args.package or tier.get("package"), args.allow_wifi)
        headset.start(env, port, run_dir)
        if not headset.wait_connected(server_log):
            raise RuntimeError("the headset did not connect")
        # Let the clock offset estimation and the tracking history settle, as with a headset
        # that connected before the application started
        settle = (
            args.settle if args.settle is not None else run.get("settle", tier.get("settle", 3))
        )
        log(f"{run['name']}: headset connected, waiting {settle} s")
        time.sleep(settle)

        runner = run.get("runner", "cts")
        if runner == "wivrn":
            # WiVRn's own OpenXR tests, told what the virtual headset reports
            cts_cmd = [
                str(build / "test/wivrn-xr-tests"),
                "--loader",
                str(find_loader(cts) or "libopenxr_loader.so.1"),
                "--junit",
                str(run_dir / "junit.xml"),
                *profile.get("args", []),
            ]
            if isinstance(headset, VirtualHeadset):
                cts_cmd += ["--headset-stats", str(headset.stats)]
            for t in (args.tests or run.get("tests", "")).split(","):
                if t:
                    cts_cmd += ["--test", t]
        else:
            cts_cmd = [str(cts), args.tests or run.get("tests", "~[interactive]")]
            if plugin == "headless":
                cts_cmd += ["-E", "XR_MND_headless"]
            else:
                cts_cmd += ["-G", plugin]
            cts_cmd += list(run.get("cts_args", []))
            cts_cmd += [
                "--reporter",
                f"junit::out={run_dir / 'junit.xml'}",
                "--reporter",
                f"console::out={run_dir / 'cts.log'}::colour-mode=none",
            ]
        if args.wrapper:
            cts_cmd = shlex.split(args.wrapper) + cts_cmd
        cts_env = dict(env.env, XR_RUNTIME_JSON=str(runtime_manifest(build)))
        for item in args.env:
            key, _, value = item.partition("=")
            cts_env[key] = value
        (run_dir / "cts.cmd").write_text(" ".join(cts_cmd) + "\n")
        log(f"{run['name']}: {' '.join(cts_cmd[1:])}")
        timeout = run.get("timeout", tier.get("timeout", 3600))
        with open(run_dir / "cts.stderr", "w") as f:
            cts_proc = subprocess.Popen(
                cts_cmd,
                cwd=cts.parent,
                stdout=f,
                stderr=f,
                start_new_session=True,
                env=cts_env,
            )
            deadline = time.monotonic() + timeout
            while (status := cts_proc.poll()) is None:
                problem = run_problem(server, headset, server_log)
                if problem or time.monotonic() > deadline:
                    kill_group(cts_proc)
                    result.errors.append(problem or f"the tests did not finish in {timeout} s")
                    break
                time.sleep(1)
            result.cts_exit = status

        if not result.errors and (problem := run_problem(server, headset, server_log)):
            result.errors.append(problem)
    except (RuntimeError, OSError) as e:
        result.errors.append(str(e))
    finally:
        if headset:
            try:
                result.headset = headset.stop()
            except (RuntimeError, OSError, subprocess.TimeoutExpired) as e:
                result.errors.append(f"stopping the headset: {e}")
        kill_group(server)
        env.close()

    result.tests, result.details = parse_junit(run_dir / "junit.xml")
    if not result.tests and not result.errors:
        result.errors.append("no test results, see cts.log and cts.stderr")
    result.errors += scan_server_log(server_log)

    if result.headset.get("client_alive") is False:
        result.errors.append("the client on the headset was not running at the end of the tests")
    for crash in result.headset.get("client_crashes", []):
        result.errors.append(f"client crash in logcat: {crash}")

    min_frames = run.get("min_video_frames", tier.get("min_video_frames", 0))
    if plugin != "headless" and min_frames and result.headset:
        frames = result.headset.get("video_frames", 0)
        if frames < min_frames:
            result.errors.append(
                f"the headset received {frames} video frames, expected at least {min_frames}"
            )

    classify(result, suites, tier_name)
    result.duration = time.monotonic() - start
    (run_dir / "result.json").write_text(json.dumps(result.__dict__, indent=2, default=str))
    return result


def write_summary(out: Path, tier_name: str, results: list[RunResult], coverage, strict) -> str:
    lines = [f"## OpenXR CTS: tier `{tier_name}`", ""]
    lines.append(
        "| Run | Plugin | Profile | Pass | Fail | Expected fail | Unexpected pass | Skip | Time |"
    )
    lines.append("|---|---|---|---|---|---|---|---|---|")
    for r in results:
        c = r.counts()
        lines.append(
            f"| {r.name} | {r.plugin} | {r.profile} | {c.get('PASS', 0)} | {c.get('FAIL', 0)} "
            f"| {c.get('XFAIL', 0)} | {c.get('XPASS', 0)} | {c.get('SKIP', 0)} "
            f"| {r.duration:.0f} s |"
        )
    lines.append("")
    for r in results:
        failed = [t for t, s in sorted(r.tests.items()) if s == "FAIL"]
        xpass = [t for t, s in sorted(r.tests.items()) if s == "XPASS"]
        if r.errors or failed or xpass:
            lines.append(f"### {r.name}")
            for e in r.errors:
                lines.append(f"- error: {e}")
            for t in failed:
                first = r.details.get(t, [""])[0].splitlines()
                lines.append(f"- FAIL `{t}`: {' '.join(first[:4])[:300]}")
            for t in xpass:
                lines.append(f"- unexpected pass `{t}` (remove it from the expected failures)")
            lines.append("")
    if coverage:
        rows, complete = coverage
        lines.append("### Extension coverage")
        lines.append("")
        lines.append("| Extension | Tests | Outcome |")
        lines.append("|---|---|---|")
        for ext, kind, how, outcome in rows:
            lines.append(f"| {ext} | {how} | {outcome or kind} |")
        if not complete:
            lines.append("")
            lines.append("Extensions marked MISSING need an entry in `test/cts/suites.toml`.")
        lines.append("")
    ok = all(r.ok(strict) for r in results) and (not coverage or coverage[1])
    lines.append(f"**Result: {'PASS' if ok else 'FAIL'}**")
    text = "\n".join(lines) + "\n"
    (out / "summary.md").write_text(text)
    return text


def cmd_run(args, suites) -> int:
    tier = suites["tiers"].get(args.tier)
    if tier is None:
        raise SystemExit(f"unknown tier {args.tier}, known: {', '.join(suites['tiers'])}")
    build = Path(args.build_dir).resolve()
    cts = cts_binary(Path(args.cts))
    for tool in ("dbus-daemon",):
        if not shutil.which(tool):
            raise SystemExit(f"{tool} is required")
    if not (build / "test/wivrn-virtual-headset").exists() and tier.get("headset") != "device":
        raise SystemExit(
            f"{build}/test/wivrn-virtual-headset not found, configure with -DWIVRN_BUILD_TEST=ON"
        )

    runs = tier["runs"]
    if args.run:
        runs = [r for r in runs if r["name"] in args.run]
        if not runs:
            raise SystemExit(f"no run named {args.run} in tier {args.tier}")
    if args.list:
        for r in runs:
            print(
                f"{r['name']:24} plugin={r.get('plugin', tier.get('plugin', 'vulkan2')):9} "
                f"profile={r.get('profile', tier.get('profile', 'default')):10} "
                f"tests={r.get('tests', '~[interactive]')}"
            )
        return 0

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = Path(args.out or f"cts-results/{args.tier}-{stamp}").resolve()
    out.mkdir(parents=True, exist_ok=True)
    log(f"results in {out}")
    (out / "tier.json").write_text(
        json.dumps({"tier": args.tier, "cts": str(cts), "build": str(build)}, indent=2)
    )

    results = []
    for run in runs:
        result = run_one(args, suites, args.tier, tier, run, out, cts, build)
        c = result.counts()
        log(
            f"{run['name']}: {c.get('PASS', 0)} pass, {c.get('FAIL', 0)} fail, "
            f"{c.get('XFAIL', 0)} expected fail, {c.get('XPASS', 0)} unexpected pass, "
            f"{c.get('SKIP', 0)} skip, {len(result.errors)} errors ({result.duration:.0f} s)"
        )
        for e in result.errors:
            log(f"  error: {e}")
        results.append(result)

    coverage = None
    if not args.no_coverage:
        exts = runtime_extensions(runtime_manifest(build), find_loader(cts))
        (out / "extensions.json").write_text(json.dumps(exts, indent=2))
        coverage = coverage_table(exts, suites, results)

    summary = write_summary(out, args.tier, results, coverage, args.strict)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as f:
            f.write(summary)
    print(summary)
    ok = all(r.ok(args.strict) for r in results) and (not coverage or coverage[1])
    return 0 if ok else 1


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--suites", default=SUITES, type=Path, help="test definitions")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("build-cts", help="clone and build the pinned CTS")
    p.add_argument("--dest", default="openxr-cts", help="directory for the sources and build")
    p.add_argument("--cmake-arg", action="append", default=[], help="extra CMake argument")

    p = sub.add_parser("extensions", help="list advertised extensions and their coverage")
    p.add_argument("--build-dir", default="build-server", help="WiVRn server build directory")
    p.add_argument("--cts", help="CTS build directory (to use its OpenXR loader)")
    p.add_argument("--json", action="store_true")

    p = sub.add_parser("run", help="run a tier")
    p.add_argument("tier", help="ci, gpu or headset (see suites.toml)")
    p.add_argument("--build-dir", default="build-server", help="WiVRn server build directory")
    p.add_argument("--cts", default="openxr-cts/build", help="CTS build directory or binary")
    p.add_argument("--out", help="results directory (default cts-results/<tier>-<time>)")
    p.add_argument("--run", action="append", help="only this run (repeatable)")
    p.add_argument("--tests", help="override the test selection (Catch2 test spec)")
    p.add_argument(
        "--vk-driver", help='Vulkan ICD manifest for the server and tests, or "lavapipe"'
    )
    p.add_argument("--settle", type=float, help="seconds between connection and tests")
    p.add_argument("--port", type=int, help="server port (default: the tier's, else 19757)")
    p.add_argument("--package", help="client package on the headset (headset tier)")
    p.add_argument("--allow-wifi", action="store_true", help="allow a WiFi route to the headset")
    p.add_argument(
        "--wrapper", help='command prefix for the tests, e.g. "gdb -batch -ex run -ex bt --args"'
    )
    p.add_argument(
        "--env",
        action="append",
        default=[],
        help="KEY=VALUE for the tests (repeatable), e.g. __GLX_VENDOR_LIBRARY_NAME=nvidia",
    )
    p.add_argument("--strict", action="store_true", help="unexpected passes fail the run")
    p.add_argument("--no-coverage", action="store_true", help="skip the extension coverage check")
    p.add_argument("--list", action="store_true", help="print the runs and exit")

    args = parser.parse_args()
    suites = load_suites(args.suites)
    return {"build-cts": build_cts, "extensions": cmd_extensions, "run": cmd_run}[args.command](
        args, suites
    )


if __name__ == "__main__":
    sys.exit(main())
