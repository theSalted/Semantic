#!/usr/bin/env python3
"""Project-scoped Unity Play supervisor. No installation, downloads, or global daemon.

Only children created by this process can be stopped. A renewable Editor lease
keeps them warm across Play/Stop/Play; an expired lease or Editor crash releases
them. Healthy pre-existing services are reused and never terminated.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request

MODELS = {"4b": (5194, "semantic-qwen3-4b-q4km", "Qwen3-4B-Q4_K_M.gguf"),
          "14b": (5191, "semantic-qwen3-14b-q4km", "Qwen3-14B-Q4_K_M.gguf")}
HTTP = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def get_json(port, route, timeout=1):
    try:
        with HTTP.open("http://127.0.0.1:%d%s" % (port, route), timeout=timeout) as response:
            return json.load(response)
    except urllib.error.HTTPError as error:
        try:
            return json.load(error)
        except (ValueError, OSError):
            return None
    except (ValueError, OSError, TimeoutError):
        return None


def listeners(port):
    result = subprocess.run(["/usr/sbin/lsof", "-nP", "-t", "-iTCP:%d" % port, "-sTCP:LISTEN"], capture_output=True, text=True)
    return {int(x) for x in result.stdout.split() if x.isdigit()}


def atomic_json(path, value):
    temp = path.with_suffix(".tmp")
    temp.write_text(json.dumps(value, indent=2))
    temp.replace(path)


def verify_bridge(health, bank_hash, profile_hash, alias):
    if not isinstance(health, dict) or health.get("service") != "semantic-unity" or health.get("protocol") != 1:
        raise RuntimeError("Port 5192 is occupied by an unrecognized or older service. Stop its owner deliberately; no process was replaced.")
    if health.get("bankSha256", "").upper() != bank_hash or health.get("profileSha256", "").upper() != profile_hash:
        raise RuntimeError("Port 5192 serves a different dialogue bank or character profile. Stop its owner deliberately before changing projects.")
    if health.get("modelAlias") != alias:
        raise RuntimeError("Port 5192 uses a different model. Select its matching model in Tools > Semantic > Local Dialogue Settings, or stop its owner deliberately.")


class Supervisor:
    def __init__(self, root, project, owner, size, timeout=120):
        self.root, self.project, self.owner, self.size = root, project, owner, size
        self.port, self.alias, self.model_name = MODELS[size]
        self.folder = project / "Library" / "SemanticHost"
        self.folder.mkdir(parents=True, exist_ok=True)
        self.state_path, self.lease_path = self.folder / "state.json", self.folder / "lease.json"
        self.bank = project / "Assets/StreamingAssets/Semantic/pell-cask.tsv"
        self.profile = project / "Assets/StreamingAssets/Semantic/pell-cask.profile.json"
        self.bank_hash = hashlib.sha256(self.bank.read_bytes()).hexdigest().upper()
        self.profile_hash = hashlib.sha256(self.profile.read_bytes()).hexdigest().upper()
        self.children, self.attempts, self.next_start = {}, {}, {}
        self.timeout, self.deadline = timeout, time.monotonic() + timeout
        self.built, self.stopping, self.ever_ready = False, False, False
        self.epoch = None
        self.log = None
        self.dotnet = shutil.which("dotnet") or "/usr/local/share/dotnet/dotnet"
        self.last_state = None

    def status(self, phase, message, ready=False, error=""):
        state = dict(service="semantic-unity-editor", supervisorPid=os.getpid(), owner=self.owner,
                     project=str(self.project), root=str(self.root), phase=phase, message=message,
                     ready=ready, error=error, modelAlias=self.alias,
                     children={name: child.pid for name, child in self.children.items() if child.poll() is None},
                     attempts=self.attempts)
        if state != self.last_state:
            atomic_json(self.state_path, state)
            print(json.dumps(state), flush=True)
            self.last_state = json.loads(json.dumps(state))

    def lease(self):
        try:
            data = json.loads(self.lease_path.read_text())
            if data.get("owner") != self.owner:
                return False
            if data.get("epoch") != self.epoch:
                self.epoch = data.get("epoch")
                self.attempts.clear()
            return float(data["expiresAt"]) > time.time()
        except (OSError, ValueError, KeyError, TypeError):
            return False

    def preflight(self):
        required = [self.root / "unity-host/Semantic.UnityHost.csproj", self.root / "scripts/run-local-llm.sh",
                    self.root / "build/libSemantic.Native.dylib", self.root / "build-llm/bin/llama-server",
                    self.root / "models/embeddinggemma-2-Q8_0.gguf", self.root / "models" / self.model_name,
                    Path(self.dotnet)]
        for path in required:
            if not path.is_file():
                raise RuntimeError("Missing prerequisite: %s. Restore this existing dependency or choose the correct Semantic checkout in Tools > Semantic > Local Dialogue Settings. No download was attempted." % path)

    def launch(self, name, command):
        old = self.children.get(name)
        if old is not None and old.poll() is None:
            return False
        count = self.attempts.get(name, 0)
        if count >= 3:
            raise RuntimeError("%s exited repeatedly (initial start plus two recovery attempts). See %s; fix the cause, then click Retry or press Play again." % (name, self.folder / "service.log"))
        if time.monotonic() < self.next_start.get(name, 0):
            return False
        self.attempts[name] = count + 1
        environment = os.environ.copy()
        if name == "bridge": environment["Logging__LogLevel__Default"] = "Warning"
        self.children[name] = subprocess.Popen(command, cwd=str(self.root), stdout=self.log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=environment)
        self.next_start[name] = time.monotonic() + (2 if count == 0 else 5)
        return True

    def step(self):
        bridge = get_json(5192, "/health")
        bridge_pids = listeners(5192)
        if bridge is not None:
            verify_bridge(bridge, self.bank_hash, self.profile_hash, self.alias)
        elif bridge_pids and not self.owns_listener("bridge", bridge_pids):
            raise RuntimeError("Port 5192 is occupied but does not answer the Semantic health check. Its owner was left running; see the port owner before retrying.")
        model = get_json(self.port, "/health")
        model_pids = listeners(self.port)
        model_ready = model is not None and model.get("status") == "ok"
        if model_ready:
            identities = get_json(self.port, "/v1/models") or {}
            if not any(x.get("id") == self.alias for x in identities.get("data", [])):
                raise RuntimeError("Model port %d serves a different model. Its process was left running; select the matching model or stop its owner deliberately." % self.port)
        elif model_pids and not self.owns_listener("model", model_pids):
            # A known llama.cpp server may still be loading; allow the same bounded readiness wait.
            if not model or model.get("status") not in ("loading model", "loading"):
                raise RuntimeError("Model port %d is occupied by an unhealthy or unrecognized process. It was not stopped." % self.port)
        if bridge is not None and bridge.get("ready") and model_ready:
            self.deadline = None
            self.ever_ready = True
            self.status("ready", "Dialogue ready", True)
            return
        if self.deadline is None:
            self.deadline = time.monotonic() + self.timeout
        if time.monotonic() > self.deadline:
            raise RuntimeError("Dialogue did not become ready within %d seconds. See %s, fix the reported dependency or occupied port, then click Retry." % (self.timeout, self.folder / "service.log"))
        if not model_pids:
            self.launch("model", ["/bin/bash", str(self.root / "scripts/run-local-llm.sh"), "--llm-size", self.size])
        if bridge is None and not bridge_pids:
            if not self.built:
                build = self.children.get("build")
                if build is None:
                    self.launch("build", [self.dotnet, "build", str(self.root / "unity-host/Semantic.UnityHost.csproj"), "-c", "Release", "--nologo"])
                elif build.poll() is not None:
                    if build.returncode != 0:
                        raise RuntimeError("Semantic host build failed. See %s; install/fix the existing .NET 10 SDK or framework build, then click Retry." % (self.folder / "service.log"))
                    self.built = True
            if self.built and model_ready:
                self.launch("bridge", [self.dotnet, str(self.root / "unity-host/bin/Release/net10.0/Semantic.UnityHost.dll"),
                    "--semantic-root", str(self.root), "--bank", str(self.bank), "--character-profile", str(self.profile),
                    "--port", "5192", "--llm-size", self.size, "--owner-id", self.owner])
        self.status("recovering" if self.ever_ready else "starting", "Reconnecting dialogue…" if self.ever_ready else "Starting local dialogue…")

    def owns_listener(self, name, pids):
        child = self.children.get(name)
        return child is not None and child.poll() is None and pids == {child.pid}

    def cleanup(self):
        # Popen owns an unreaped child, so its live PID cannot have been recycled.
        # Additionally confirm parent PID and expected executable/arguments before signaling.
        for name in ("bridge", "model", "build"):
            child = self.children.get(name)
            if child is None or child.poll() is not None:
                continue
            result = subprocess.run(["/bin/ps", "-p", str(child.pid), "-o", "ppid=", "-o", "command="], capture_output=True, text=True)
            parts = result.stdout.strip().split(None, 1)
            expected = (self.owner if name == "bridge" else self.model_name if name == "model" else "Semantic.UnityHost.csproj")
            command_ok = len(parts) == 2 and (expected in parts[1] or (name == "model" and str(self.root / "scripts/run-local-llm.sh") in parts[1]))
            if not command_ok or parts[0] != str(os.getpid()):
                print("Refusing cleanup: child identity no longer matches %s PID %s" % (name, child.pid), flush=True)
                continue
            port = 5192 if name == "bridge" else self.port if name == "model" else None
            print("Stopping owned %s PID %s; listeners=%s" % (name, child.pid, sorted(listeners(port)) if port else []), flush=True)
            child.terminate()
            try:
                child.wait(timeout=8)
            except subprocess.TimeoutExpired:
                # Still our unreaped child; bounded final cleanup after graceful termination.
                child.kill()
                child.wait(timeout=5)

    def run(self):
        lock = open(self.folder / "supervisor.lock", "a")
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return 0
        log_path = self.folder / "service.log"
        if log_path.exists() and log_path.stat().st_size > 5_000_000:
            log_path.replace(self.folder / "service.previous.log")
        self.log = open(log_path, "a", buffering=1)
        failed = False
        try:
            self.preflight()
            while not self.stopping and self.lease():
                self.step()
                time.sleep(5 if self.last_state and self.last_state.get("ready") else 1)
        except Exception as error:
            failed = True
            self.status("failed", "Dialogue could not start", error=str(error))
        finally:
            self.cleanup()
            if not failed:
                self.status("stopped", "Editor lease ended; owned services stopped")
            self.log.close()
            lock.close()
        return 1 if failed else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", required=True)
    parser.add_argument("--owner", required=True)
    parser.add_argument("--llm-size", choices=MODELS, default="4b")
    args = parser.parse_args()
    supervisor = Supervisor(Path(__file__).resolve().parents[1], Path(args.project).resolve(), args.owner, args.llm_size)
    def stop(*_):
        supervisor.stopping = True
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    return supervisor.run()

if __name__ == "__main__":
    sys.exit(main())
