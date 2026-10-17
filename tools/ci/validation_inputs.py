"""Stage the same source-independent test inputs for PR and release validation."""
from pathlib import Path
import shutil


PYTHON_EXAMPLES = (
    "circular_orbit", "time_frames", "eop_state", "sun_moon", "two_body_scenario",
    "propagators", "access", "report_templates", "mission",
)


def stage_validation_inputs(source: Path, destination: Path):
    # Preserve repository-relative paths used by the tests, without copying the
    # Python package or C++ implementation that must come from the installation.
    for relative in (
        "tests/packaging/sdk_consumer", "tests/reference/time_frames",
        "tests/reference/propagators",
        "tests/integration/fixtures", "bindings/python/tests", "examples/python",
        "data/samples/eop", "data/samples/time", "data/samples/spice",
    ):
        shutil.copytree(source / relative, destination / relative,
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    shutil.copy2(source / "tools/validation/compare_examples.py", destination)
