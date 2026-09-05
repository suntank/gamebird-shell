"""Exercise the real helper with a deterministic RetroArch stand-in."""
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile

helper = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="gamebird launch test ") as temp:
    root = Path(temp)
    db_path = root / "catalog.db"
    subprocess.run([helper, "--db", str(db_path), "--validate-all"], check=True, capture_output=True)
    fake = root / "retroarch"
    fake.write_text('''#!/usr/bin/env python3
import os, pathlib, signal, sys, re
args = sys.argv[1:]
assert args.count('--appendconfig') == 1, args
configs = args[args.index('--appendconfig') + 1].split('|')
assert len(configs) == 3, configs
assert pathlib.Path(configs[0]).read_text() == 'base config'
assert pathlib.Path(configs[1]).read_text() == 'input config'
config = pathlib.Path(configs[2]).read_text()
assert '--savestate' not in args
state_dir = re.search(r'savestate_directory = "(.*)"', config).group(1)
state = pathlib.Path(state_dir) / 'Test game.state.auto' 
expected = os.environ.get('EXPECTED_SEED')
if expected is None:
    assert not state.exists(), 'fresh launch loaded an old state'
    assert 'savestate_auto_load = "false"' in config
else:
    assert state.read_text() == expected, 'wrong resume seed'
    assert 'savestate_auto_load = "true"' in config
mode = os.environ.get('FAKE_EXIT', 'save')
if mode != 'no-save': state.write_text(os.environ.get('NEW_STATE', 'first save'))
if mode == 'signal': os.kill(os.getpid(), signal.SIGTERM)
sys.exit(1 if mode == 'error' else 0)
''')
    fake.chmod(0o700)
    core = root / "core.so"
    core.write_text("core")
    rom = root / "Test game.sfc"
    rom.write_text("rom")
    base, inputs = root / "base.cfg", root / "input.cfg"
    base.write_text("base config")
    inputs.write_text("input config")
    command = f'"{fake}" --appendconfig "{base}|{inputs}" -L "{core}" "{{rom_path}}"'
    db = sqlite3.connect(db_path)
    db.execute("INSERT INTO systems VALUES (?,?,?,?,?)", ("snes", "SNES", ".sfc", "retroarch", command))
    db.execute("INSERT INTO games (system_id,path,filename,title,sort_title,size_bytes,mtime) VALUES (?,?,?,?,?,?,?)",
               ("snes", str(rom), rom.name, "Test game", "Test game", 3, 0))
    game = db.execute("SELECT id FROM games").fetchone()[0]
    db.commit()

    def run(flag="--fresh", seed=None, output="first save", exit_mode="save", expected_code=0):
        env = os.environ.copy()
        env.pop("EXPECTED_SEED", None)
        if seed is not None:
            env["EXPECTED_SEED"] = seed
        env.update(NEW_STATE=output, FAKE_EXIT=exit_mode)
        result = subprocess.run([helper, "--db", str(db_path), "--game-id", str(game), flag],
                                env=env, capture_output=True, text=True, timeout=20)
        assert result.returncode == expected_code, result.stdout + result.stderr
        return result

    run()
    current = next((root / "continue").glob("*/current.auto"))
    previous = current.with_name("previous.auto")
    receipt = root / "continue" / f"result-{game}"
    assert current.read_text() == "first save"
    assert "Progress saved" in receipt.read_text()
    run("--resume", "first save", "second save")
    assert current.read_text() == "second save" and previous.read_text() == "first save"
    run("--resume-backup", "first save", "third save")
    assert current.read_text() == "third save" and previous.read_text() == "second save"
    db.execute("UPDATE games SET last_played=1000")
    db.commit()
    run("--fresh", output="broken", exit_mode="error", expected_code=1)
    assert current.read_text() == "third save" and previous.read_text() == "second save"
    assert db.execute("SELECT last_played FROM games").fetchone()[0] == 1000
    run("--resume", "third save", output="crashed", exit_mode="signal", expected_code=143)
    assert current.read_text() == "third save"
    run("--resume", "third save", exit_mode="no-save")
    assert "No new resume save" in receipt.read_text()
    assert current.read_text() == "third save"
    assert not list((root / "continue").glob("*/run-*")), "staging directories cleaned"
    core.write_text("different core version")
    run("--resume", expected_code=1)
    assert current.read_text() == "third save"
    db.close()
print("launch session integration passed")
