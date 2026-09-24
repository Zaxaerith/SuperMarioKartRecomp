"""Bounded desktop pacing A/B: normal audio, silent callback, no audio device.

Uses the real SDL video backend unless --video-driver is specified. Each run
creates its own log and exits after --frames. --script supplies repeatable
guest inputs for menu or race profiling.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
ROM = Path(os.environ.get('SMK_ROM', ROOT / 'Super Mario Kart (USA).sfc'))
EXE = ROOT / 'build' / 'smk_play.exe'


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--frames', type=int, default=180)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--video-driver', help='SDL_VIDEODRIVER override, e.g. dummy')
    p.add_argument('--audio-driver', help='SDL_AUDIODRIVER override, e.g. dummy')
    p.add_argument('--modes', nargs='+', choices=['normal', 'silent', 'off'],
                   default=['normal', 'silent', 'off'])
    p.add_argument('--script', help='Optional headless-format input script for race profiling')
    args = p.parse_args()
    if not 1 <= args.frames <= 3600:
        p.error('--frames must be in 1..3600')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    results = []
    for mode in args.modes:
        log = out / (mode + '.perf.log')
        env = dict(os.environ)
        env.update(SMK_PERF_LOG=str(log), SMK_DIAG_FRAMES=str(args.frames),
                   SMK_DIAG_AUDIO=mode)
        if args.script:
            env['SMK_DIAG_SCRIPT'] = args.script
        if args.video_driver:
            env['SDL_VIDEODRIVER'] = args.video_driver
        if args.audio_driver:
            env['SDL_AUDIODRIVER'] = args.audio_driver
        started = time.monotonic()
        try:
            proc = subprocess.run([str(EXE), str(ROM)], cwd=out, env=env,
                                  capture_output=True, text=True,
                                  errors='replace', timeout=max(60, args.frames))
            code, stdout, stderr = proc.returncode, proc.stdout, proc.stderr
        except subprocess.TimeoutExpired as e:
            code, stdout, stderr = 'timeout', str(e.stdout), str(e.stderr)
        (out / (mode + '.stdout.log')).write_text(stdout, encoding='utf-8')
        (out / (mode + '.stderr.log')).write_text(stderr, encoding='utf-8')
        result = dict(mode=mode, exit_code=code,
                      seconds=round(time.monotonic() - started, 3),
                      perf_log=str(log), perf_lines=log.read_text(
                          encoding='utf-8', errors='replace').splitlines()
                          if log.exists() else [])
        results.append(result)
        print(mode, code, result['seconds'], flush=True)
        if code != 0:
            break
    report = dict(exe_sha256=hashlib.sha256(EXE.read_bytes()).hexdigest(),
                  rom_sha256=hashlib.sha256(ROM.read_bytes()).hexdigest(),
                  frames=args.frames, video_driver=args.video_driver or 'default',
                  audio_driver=args.audio_driver or 'default',
                  script=args.script, results=results)
    (out / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report, indent=2))
    return 0 if len(results) == len(args.modes) and all(
        r['exit_code'] == 0 for r in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
