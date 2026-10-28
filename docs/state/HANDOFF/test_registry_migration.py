"""Resolve only the reviewed explicit-test-list to #115 discovery migration."""
from pathlib import Path
import re


def resolve_test_registry_conflict(path):
    if path != "tests/CMakeLists.txt":
        return False
    text = Path(path).read_text(encoding="utf-8")
    pattern = re.compile(r"^<<<<<<< [^\n]*\n(.*?)^=======\n(.*?)^>>>>>>> [^\n]*\n", re.M | re.S)
    variable = "$" + "{CMAKE_CURRENT_SOURCE_DIR}"
    glob = "\n".join([
        "# Every tests/<area>/test_*.cpp is part of osp_tests; adding a block adds a file, not a line here (#115).",
        "# tests/app (needs JUCE) and tests/bench are built separately.",
        "file(GLOB OSP_TEST_SOURCES CONFIGURE_DEPENDS",
        *[f'    "{variable}/{area}/test_*.cpp"' for area in ("dsp", "engine", "measure", "rt")],
        f'    "{variable}/tools/test_*.cpp")',
        "add_executable(osp_tests ${OSP_TEST_SOURCES})",
        "",
    ])
    normalize = lambda value: " ".join(value.split())
    count = 0

    def replace(match):
        nonlocal count
        for old, new in (match.groups(), match.groups()[::-1]):
            if normalize(new) != normalize(glob):
                continue
            if not re.fullmatch(r"add_executable\(osp_tests\s+(?:(?:dsp|engine|measure|rt|tools)/test_[A-Za-z0-9_]+\.cpp\s*)+\)\s*", old):
                continue
            paths = re.findall(r"(?:dsp|engine|measure|rt|tools)/test_[A-Za-z0-9_]+\.cpp", old)
            if not paths or not all(Path("tests", name).is_file() for name in paths):
                continue
            count += 1
            return new
        return match.group(0)

    fixed = pattern.sub(replace, text)
    if count == 0 or any(marker in fixed for marker in ("<<<<<<<", "=======", ">>>>>>>")):
        return False
    Path(path).write_text(fixed, encoding="utf-8", newline="\n")
    return True
