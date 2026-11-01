#!/usr/bin/env python3
"""Exercise CLI failure paths without ever opening a real display."""

import pathlib
import signal
import subprocess
import sys
import tempfile
import time


def main() -> None:
    renderer = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="gamebird-splash-cli-") as directory:
        temporary = pathlib.Path(directory)
        image = temporary / "splash.rgb565"
        image.write_bytes(bytes(240 * 240 * 2))
        missing = temporary / "no-framebuffer"

        def run(expected: int, *arguments: str) -> subprocess.CompletedProcess:
            result = subprocess.run(
                [renderer, *arguments], capture_output=True, text=True, timeout=3
            )
            assert result.returncode == expected, (result.args, result.returncode, result.stderr)
            return result

        assert "image OK" in run(0, "--check-image", str(image)).stdout
        assert "Usage:" in run(0, "--help").stdout
        run(2, "--timeout-ms", "-1")
        run(2, "--timeout-ms", "10001")
        run(2, "--image")
        run(2, "--check-image", str(image), "--timeout-ms", "0")

        common = ("--image", str(image), "--fbdev", str(missing))
        run(3, *common, "--timeout-ms", "0")
        start = time.monotonic()
        run(3, *common, "--timeout-ms", "80")
        elapsed = time.monotonic() - start
        assert 0.06 <= elapsed < 2, elapsed

        # SIGTERM must run normal cleanup, not default-kill a process that may
        # have placed the console in graphics mode. No display is opened here.
        with subprocess.Popen(
            [renderer, *common, "--timeout-ms", "10000"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        ) as waiting:
            time.sleep(0.1)
            waiting.terminate()
            _, stderr = waiting.communicate(timeout=2)
            assert waiting.returncode == 128 + signal.SIGTERM, (waiting.returncode, stderr)
            assert "interrupted by signal" in stderr

        rejected = run(1, "--image", str(image), "--fbdev", str(image), "--timeout-ms", "0")
        assert "character device" in rejected.stderr
        assert image.read_bytes() == bytes(240 * 240 * 2)
        image.write_bytes(b"truncated")
        assert "115200-byte" in run(1, "--check-image", str(image)).stderr
        run(1, "--image", str(image), "--fbdev", str(missing), "--timeout-ms", "0")

    print("splash CLI tests passed")


if __name__ == "__main__":
    main()
