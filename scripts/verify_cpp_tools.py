"""Prove verification tools reject known defects; fixtures stay under ignored build/."""

import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from check_cpp import ROOT, run, toolchain, write_receipt


def main():
    tools = toolchain()
    log_dir = ROOT / "build" / "reports" / "tool-verification"
    log_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="fixtures-", dir=log_dir) as directory:
        fixtures = Path(directory)
        null = fixtures / "null.cpp"
        null.write_text("int main() { int* pointer = nullptr; return *pointer; }\n")
        unused = fixtures / "unused.cpp"
        unused.write_text("int main() { int unused = 7; return 0; }\n")
        unformatted = fixtures / "unformatted.cpp"
        unformatted.write_text("int main(){return 0;}\n")
        checks = [
            (
                "compiler-warning",
                [
                    tools["clang++"],
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsyntax-only",
                    unused,
                ],
                "unused-variable",
            ),
            (
                "formatting",
                [tools["clang-format"], "--dry-run", "--Werror", unformatted],
                "clang-format-violations",
            ),
            (
                "static-null",
                [tools["clang-tidy"], null, "--", "-std=c++20"],
                "clang-analyzer-core.NullDereference",
            ),
            (
                "cppcheck-null",
                [tools["cppcheck"], "--error-exitcode=1", null],
                "nullPointer",
            ),
        ]
        with ThreadPoolExecutor(max_workers=4) as pool:
            futures = [
                pool.submit(run, label, command, log_dir, expect_failure=diagnostic)
                for label, command, diagnostic in checks
            ]
            results = [future.result() for future in futures]

        runtime_probes = [
            (
                "address",
                "address",
                "AddressSanitizer: heap-buffer-overflow",
                (
                    "#include <memory>\n#include <cstddef>\n"
                    "int main(int argc, char**) { auto values = std::make_unique<int[]>(4); "
                    "return values[static_cast<std::size_t>(argc + 4)]; }\n"
                ),
            ),
            (
                "undefined",
                "undefined",
                "runtime error: signed integer overflow",
                (
                    "#include <limits>\nint main(int argc, char**) { "
                    "return std::numeric_limits<int>::max() + argc; }\n"
                ),
            ),
            (
                "thread",
                "thread",
                "ThreadSanitizer: data race",
                (
                    "#include <barrier>\n#include <thread>\n"
                    "int main() { int value = 0; std::barrier start{3}; "
                    "auto change = [&] { start.arrive_and_wait(); "
                    "for (int i = 0; i < 1000; ++i) { ++value; std::this_thread::yield(); } }; "
                    "std::thread first(change); std::thread second(change); "
                    "start.arrive_and_wait(); first.join(); second.join(); return value == 0; }\n"
                ),
            ),
        ]

        def runtime_probe(name, sanitizer, diagnostic, source):
            path = fixtures / f"{name}.cpp"
            binary = fixtures / name
            path.write_text(source)
            compiled = run(
                f"{name}-compile",
                [
                    tools["clang++"],
                    "-std=c++20",
                    "-O0",
                    "-g",
                    "-pthread",
                    f"-fsanitize={sanitizer}",
                    "-fno-sanitize-recover=all",
                    path,
                    "-o",
                    binary,
                ],
                log_dir,
            )
            if not compiled["passed"]:
                return [compiled]
            # All three race runs must detect the fault; this is not a retry-until-pass.
            attempts = 3 if name == "thread" else 1
            return [compiled] + [
                run(
                    f"{name}-detection-{attempt}",
                    [binary],
                    log_dir,
                    expect_failure=diagnostic,
                )
                for attempt in range(attempts)
            ]

        with ThreadPoolExecutor(max_workers=3) as pool:
            futures = [pool.submit(runtime_probe, *probe) for probe in runtime_probes]
            for future in futures:
                results.extend(future.result())
    passed = write_receipt(
        log_dir / "receipt.json",
        tools,
        results,
        "Deliberately faulty disposable fixtures verify detector activation; not application tests.",
    )
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
