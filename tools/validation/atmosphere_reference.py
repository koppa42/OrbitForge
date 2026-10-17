"""Reproduce independent density fixtures; requires MSVC tools or GCC in PATH.

Downloads pinned, hash-checked sources into an explicit output directory, never
links OrbitForge, and writes candidates for review (does not replace fixtures).
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--msvc", action="store_true", help="Use cl in a developer shell")
    args = parser.parse_args()
    tools = Path(__file__).resolve().parent
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    upstream = output / "upstream"
    upstream.mkdir(exist_ok=True)
    for record in json.loads((tools / "density_sources.json").read_text()):
        path = upstream / record["file"]
        if not path.exists():
            request = urllib.request.Request(record["url"], headers={"User-Agent": "OrbitForge-validation"})
            with urllib.request.urlopen(request, timeout=60) as response:
                raw = response.read()
            if hashlib.sha256(raw).hexdigest() != record["sha256"]:
                raise ValueError(f"Unexpected source hash: {record['file']}")
            path.write_bytes(raw)
        if hashlib.sha256(path.read_bytes()).hexdigest() != record["sha256"]:
            raise ValueError(f"Unexpected cached source hash: {record['file']}")
    for model, producer in (("nrlmsise00", "nrlmsise_reference.c"),
                            ("nrlmsise00_ap", "nrlmsise_ap_reference.c"),
                            ("jacchia_roberts", "jacchia_roberts_reference.cpp")):
        executable = output / (model + (".exe" if os.name == "nt" else ""))
        sources = [str(tools / producer)]
        if model.startswith("nrlmsise00"):
            sources += [str(upstream / "nrlmsise-00.c"), str(upstream / "nrlmsise-00_data.c")]
        if args.msvc:
            command = ["cl", "/nologo", "/O2", "/EHsc", "/std:c++20", f"/I{upstream}",
                       f"/I{tools / 'gmat_jr_stubs'}", *sources, f"/Fe:{executable}"]
        else:
            command = ["gcc" if model.startswith("nrlmsise00") else "g++", "-O2", f"-I{upstream}",
                       f"-I{tools / 'gmat_jr_stubs'}", *sources, "-lm", "-o", str(executable)]
        subprocess.run(command, cwd=output, check=True)
        raw = subprocess.check_output([str(executable)], cwd=output).replace(b"\r\n", b"\n")
        (output / f"{model}.txt").write_bytes(raw)
        print(model, hashlib.sha256(raw).hexdigest())


if __name__ == "__main__":
    main()
