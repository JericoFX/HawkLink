import argparse
import datetime
import hashlib
import json
import subprocess
from pathlib import Path


ROOT = Path(r"F:\HawkenServer")
GAME = Path(r"F:\SteamLibrary\steamapps\common\Hawken\Binaries\Win64")
EXPECTED_PROBE = "579e0b32b228fd318982eb8ec4c0abde32ace0dfa64ce4f4ec62b1d574ec227d"
ORIGINALS = {
    "HawkenGame-Win64-Shipping.exe": "4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2",
    "Meteor.Sdk.Cpp.Win32.Client64.dll": "f63fef02ffb517ca7844a769d03412ad82bd22198397302ee3c090c63a970c57",
    "StormConsumerSettings.ini": "08eda2cb20539996b7c1a91480c1ee2f64e02743c9bbee0bfc4f3543d8fec3d9",
    "steam_api64.dll": "38f6a1dd1e738e142160322638dae7b88dabcc30eea3f15c58eb08c5341f9b29",
    "steam_appid.txt": "7f8af64056cc012f81841069a6bb93ad471e5f738a82e05a4981030a6da34233",
}


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--logon", action="store_true")
    options = parser.parse_args()
    timeout = 45 if options.logon else 10
    executable = ROOT / "build/windows-msvc/hawken-steam-runtime-probe.exe"
    before = {name: digest(GAME / name) for name in ORIGINALS}
    if digest(executable) != EXPECTED_PROBE or before != ORIGINALS:
        raise RuntimeError("Known probe/original hashes do not match.")
    prefix = "steam-logon-probe-local-" if options.logon else "steam-runtime-probe-local-"
    session = prefix + datetime.datetime.now(
        datetime.timezone.utc).strftime("%Y%m%d-%H%M%S")
    directory = ROOT / "implementation/evidence" / session
    directory.mkdir(exist_ok=False)
    result_path = directory / "result.json"
    record = {
        "schema": "hawken.steam-runtime-logon.v1" if options.logon else "hawken.steam-runtime-availability.v1",
        "session": session,
        "utc_start": utc(),
        "probe_sha256": EXPECTED_PROBE,
        "original_hashes_before": before,
        "timeout_seconds": timeout,
        "stdout_and_stderr_discarded": True,
        "logon_requested": options.logon,
        "identities_requested_only_in_memory": options.logon,
        "identities_logged_or_persisted": False,
        "tickets_or_authentication_requested": False,
        "authentication_verified": False,
        "gameplay_verified": False,
    }
    timed_out = False
    command = [str(executable), "--result", str(result_path)]
    if options.logon:
        command.append("--logon")
    with subprocess.Popen(
        command, cwd=directory,
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        creationflags=subprocess.CREATE_NO_WINDOW,
    ) as process:
        record["pid"] = process.pid
        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            process.kill()  # Only this owned diagnostic child and its Windows handle.
            process.wait(timeout=5)
        record["exit_code"] = process.returncode
    record["utc_end"] = utc()
    record["timed_out"] = timed_out
    record["report_created"] = result_path.is_file()
    record["report_parseable"] = False
    if result_path.is_file():
        record["report_sha256"] = digest(result_path)
        try:
            result = json.loads(result_path.read_text(encoding="utf-8"))
            record["report_parseable"] = isinstance(result, dict)
        except (ValueError, UnicodeError):
            pass
    after = {name: digest(GAME / name) for name in ORIGINALS}
    record["original_hashes_after"] = after
    record["originals_unchanged"] = after == before
    (directory / "launch-review.json").write_text(
        json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"session": session, "pid": record["pid"],
                      "exit_code": record["exit_code"], "timed_out": timed_out,
                      "originals_unchanged": record["originals_unchanged"]}))
    return 0 if record["exit_code"] == 0 and not timed_out and after == before else 1


if __name__ == "__main__":
    raise SystemExit(main())
