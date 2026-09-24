"""Print sampled game-state changes for one bounded headless input route."""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
ROM = Path(os.environ.get('SMK_ROM', ROOT / 'Super Mario Kart (USA).sfc'))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--frames', type=int, default=1600)
    parser.add_argument('--script', required=True)
    args = parser.parse_args()
    if not 1 <= args.frames <= 3600:
        parser.error('frames must be 1..3600')
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('SMK_', 'SNESRECOMP_'))}
    env['SMK_SCRIPT'] = args.script
    run = subprocess.run([str(ROOT / 'build/smk_headless.exe'), str(ROM),
                          str(args.frames)], cwd=ROOT, env=env,
                         capture_output=True, text=True, errors='replace',
                         timeout=180)
    rows = [json.loads(line) for line in run.stdout.splitlines()
            if line.startswith('{"frame":')]
    states = [(row['frame'], row['mode'], row['state']) for row in rows]
    changes = [state for i, state in enumerate(states)
               if i == 0 or state[1:] != states[i - 1][1:]]
    print('exit', run.returncode, 'changes', changes,
          'last', states[-1] if states else None)
    if run.returncode:
        print(run.stderr[-1000:])
    return 0 if run.returncode == 0 else 1


if __name__ == '__main__':
    raise SystemExit(main())
