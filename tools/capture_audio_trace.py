"""Bounded LOCAL Shield Perfetto capture. Never uploads a trace.

Arm app marker/mode BEFORE launch. Start this before selecting the matched source.
Do not treat a trace without actual source/decoder/coverage verification as an A/B.
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import subprocess
import time

ADB = Path.home()/"AppData/Local/Android/Sdk/platform-tools/adb.exe"
PACKAGE = "space.nuvio.nativelegacy.sharing"


def config(seconds):
    if not 2 <= seconds <= 600:
        raise ValueError("duration must be 2..600 seconds")
    events = ["sched/sched_switch", "sched/sched_waking", "sched/sched_wakeup",
              "power/cpu_frequency", "power/cpu_idle", "binder/binder_transaction",
              "binder/binder_transaction_received"]
    return '\n'.join([
        'buffers { size_kb: 32768 fill_policy: RING_BUFFER }',
        'data_sources { config { name: "linux.ftrace" ftrace_config {',
        *[f'ftrace_events: "{event}"' for event in events],
        *[f'atrace_categories: "{category}"' for category in ["audio", "video", "binder_driver", "hal", "dalvik", "gfx"]],
        f'atrace_apps: "{PACKAGE}"',
        '} } }',
        'data_sources { config { name: "linux.process_stats" process_stats_config { scan_all_processes_on_start: true proc_stats_poll_ms: 1000 } } }',
        f'duration_ms: {seconds*1000}',
        'write_into_file: true',
        'file_write_period_ms: 5000',
        'flush_period_ms: 5000',
        'max_file_size_bytes: 1073741824',
    ])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', default='192.168.0.119:5555')
    parser.add_argument('--seconds', type=int, default=330)
    parser.add_argument('--label', required=True)
    parser.add_argument('--mode', choices=['thumbnails-on','thumbnails-off','smoke'],required=True)
    parser.add_argument('--cleanup-markers', action='store_true')
    args = parser.parse_args()
    if not re.fullmatch(r'[a-zA-Z0-9_.-]{1,40}', args.label):
        parser.error('invalid label')
    content = config(args.seconds)
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    output = Path('build/audio-traces')/(stamp+'-'+args.label)
    output.mkdir(parents=True)
    cfg = output/'config.pbtxt'; cfg.write_text(content)
    adb = [str(ADB), '-s', args.serial]
    def run(*arguments, input_text=None):
        return subprocess.run(adb+list(arguments), input=input_text, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=180 if arguments[0]=='pull' else 20, check=True)
    pid=''
    until=time.monotonic()+10
    while time.monotonic()<until:
        result=subprocess.run(adb+['shell','pidof',PACKAGE],capture_output=True,text=True,timeout=10)
        pid=result.stdout.strip()
        if pid.isdigit(): break
        time.sleep(.25)
    if not pid.isdigit():
        raise RuntimeError('one app process required')
    startup=run('logcat','-d','--pid='+pid,'-v','threadtime','-s','AudioFeedTrace:I')
    (output/'startup-metadata.log').write_text(startup.stdout)
    remote_trace = f'/data/misc/perfetto-traces/nuvio-audio-{stamp}-{args.label}.pftrace'
    private_log = (output/'playback-private.log').open('wb')
    logger = subprocess.Popen(adb+['logcat','--pid='+pid,'-T','1','-v','threadtime'],stdout=private_log,stderr=subprocess.STDOUT)
    receipt = {'label':args.label,'modeRequested':args.mode,'pid':int(pid),'serial':args.serial,'seconds':args.seconds,
               'localOnly':True,'traceFileCapBytes':1073741824,'status':'starting'}
    receipt_path = output/'receipt.json'
    def save():
        receipt_path.write_text(json.dumps(receipt,indent=2))
    save()
    try:
        result = run('shell','perfetto','--txt','-c','-','-o',remote_trace,'--background',input_text=content)
        (output/'perfetto-start.log').write_text(result.stdout+result.stderr)
        receipt['status']='recording'; save()
        print('RECORDING '+str(output),flush=True)
        time.sleep(args.seconds+3)
        print('RETRIEVING '+str(output),flush=True)
        run('pull',remote_trace,str(output/'system.pftrace'))
        receipt['traceBytes']=(output/'system.pftrace').stat().st_size
        receipt['status']='captured-unanalysed' if receipt['traceBytes']>0 else 'empty-trace'
        remote_size=int(run('shell','stat','-c','%s',remote_trace).stdout.strip())
        if remote_size==receipt['traceBytes'] and remote_size>0:
            run('shell','rm','-f',remote_trace)
            receipt['remoteTraceRemovedAfterPull']=True
    except Exception as exc:
        receipt['status']='failed';receipt['errorType']=type(exc).__name__
        if isinstance(exc,subprocess.CalledProcessError):
            (output/'capture-error.log').write_text((exc.stdout or '')+(exc.stderr or ''))
        raise
    finally:
        logger.terminate()
        try: logger.wait(timeout=5)
        except subprocess.TimeoutExpired: logger.kill();logger.wait()
        private_log.close();save()
        if args.cleanup_markers:
            base=f'/sdcard/Android/data/{PACKAGE}/files/'
            try:
                run('shell','rm','-f',*[base+name for name in ['audio-trace-cold.enabled','audio-trace-no-thumbnails.enabled','audio-feed-trace.enabled']])
                receipt['diagnosticMarkersRemoved']=True
            except Exception:
                receipt['diagnosticMarkersRemoved']=False
            save()
    print('CAPTURED '+str(output)+' bytes='+str(receipt.get('traceBytes',0)),flush=True)


if __name__=='__main__':
    main()
