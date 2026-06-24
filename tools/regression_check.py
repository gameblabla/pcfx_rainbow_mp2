#!/usr/bin/env python3
import argparse, json, math, os, pathlib, re, struct, subprocess, tempfile, wave
from typing import Dict, List, Tuple

import numpy as np

SECTOR=2048
MAGIC=b'PCFV0001'

def rd16(b,o): return struct.unpack_from('<H',b,o)[0]
def rd32(b,o): return struct.unpack_from('<I',b,o)[0]

def parse_pcfv(path: pathlib.Path) -> Dict:
    data=path.read_bytes()[:65536]
    if data[:8] != MAGIC:
        raise SystemExit(f'{path}: not PCFV0001')
    w,h=rd16(data,8),rd16(data,10)
    fpsn,fpsd=rd16(data,12),rd16(data,14)
    frames=rd16(data,16)
    flags=rd16(data,18)
    idx_bytes=rd32(data,20)
    data_start=rd32(data,24)
    max_video_sectors=rd32(data,28)
    audio_bytes=rd32(data,32)
    audio_sectors=rd32(data,36)
    chunk_sectors=rd32(data,44)
    sample_rate=rd32(data,52)
    return dict(width=w,height=h,fps_num=fpsn,fps_den=fpsd,fps=(fpsn/fpsd if fpsd else 0.0),frames=frames,
                flags=flags,index_bytes=idx_bytes,data_start_sector=data_start,max_video_sectors=max_video_sectors,
                audio_bytes=audio_bytes,audio_sectors=audio_sectors,chunk_sectors=chunk_sectors,sample_rate=sample_rate)

def run_ffmpeg_audio(src: pathlib.Path, sr:int=16000) -> np.ndarray:
    cmd=['ffmpeg','-hide_banner','-loglevel','error','-i',str(src),'-map','0:a:0','-vn','-ac','1','-ar',str(sr),'-f','s16le','-']
    raw=subprocess.check_output(cmd)
    return np.frombuffer(raw,dtype='<i2').astype(np.float32)/32768.0

def read_wav_mono(path: pathlib.Path) -> Tuple[np.ndarray,int]:
    with wave.open(str(path),'rb') as w:
        ch=w.getnchannels(); sr=w.getframerate(); n=w.getnframes()
        raw=w.readframes(n)
        x=np.frombuffer(raw,dtype='<i2').astype(np.float32).reshape(-1,ch)/32768.0
        if ch>1: x=x.mean(axis=1)
        else: x=x[:,0]
        return x,sr

def envelope(x: np.ndarray, sr:int, win_ms:float=50.0) -> Tuple[np.ndarray,float]:
    win=max(1,int(sr*win_ms/1000.0))
    n=len(x)//win
    if n<=0: return np.zeros(0,dtype=np.float32), win/sr
    y=x[:n*win].reshape(n,win)
    e=np.sqrt(np.mean(y*y,axis=1)+1e-12)
    return e.astype(np.float32), win/sr

def active_segments(e:np.ndarray, step:float, min_start:float, min_len:float=2.0) -> List[Tuple[float,float,float]]:
    if len(e)==0: return []
    # Robust floor: ignore silence and BIOS noise; threshold 34 dB below peak, but not below absolute floor.
    peak=float(np.max(e))
    if peak <= 1e-7: return []
    threshold=max(peak*10**(-34/20), 0.0015)
    active=e>threshold
    # close short gaps under 0.25s
    gap=int(round(0.25/step))
    if gap>0:
        i=0
        while i<len(active):
            if active[i]: i+=1; continue
            j=i
            while j<len(active) and not active[j]: j+=1
            if i>0 and j<len(active) and (j-i)<=gap: active[i:j]=True
            i=j
    segs=[]; i=0
    while i<len(active):
        if not active[i]: i+=1; continue
        j=i
        while j<len(active) and active[j]: j+=1
        start=i*step; end=j*step; dur=end-start
        if start>=min_start and dur>=min_len:
            segs.append((start,end,dur))
        i=j
    return segs

def best_envelope_scale(ref:np.ndarray, ref_sr:int, emu:np.ndarray, emu_sr:int, min_start_sec:float=8.0) -> Dict:
    ref_env,step=envelope(ref, ref_sr, 50.0)
    emu_env,_=envelope(emu, emu_sr, 50.0)
    if len(ref_env) < 8 or len(emu_env) < 8:
        raise SystemExit('insufficient audio for envelope scale check')
    # Log RMS is more robust to level differences and brief silence.
    r0=np.log(ref_env+1e-5)
    e0=np.log(emu_env+1e-5)
    r0=(r0-float(np.mean(r0)))/(float(np.std(r0))+1e-9)
    e0=(e0-float(np.mean(e0)))/(float(np.std(e0))+1e-9)
    start_idx=max(0,int(round(min_start_sec/step)))
    search=e0[start_idx:]
    best=None
    # +/-10% is intentionally wide: a bad PC-FX timer setting should fail loudly.
    for scale in np.linspace(0.90,1.10,161):
        L=max(8,int(round(len(r0)*float(scale))))
        if L > len(search):
            continue
        idx=np.linspace(0,len(r0)-1,L)
        r=np.interp(idx,np.arange(len(r0)),r0)
        r=(r-float(np.mean(r)))/(float(np.std(r))+1e-9)
        corr=np.correlate(search, r, mode='valid')/float(L)
        pos=int(np.argmax(corr)); score=float(corr[pos])
        if best is None or score > best['correlation']:
            best=dict(correlation=score, scale=float(scale), offset_sec=(start_idx+pos)*step,
                      matched_duration_sec=L*step, reference_duration_sec=len(ref)/ref_sr)
    if best is None:
        raise SystemExit('could not match emulator WAV to reference envelope')
    return best

def compare_wav_to_reference(wav_path:pathlib.Path, source:pathlib.Path, expected_sr:int=16000) -> Dict:
    ref=run_ffmpeg_audio(source, expected_sr)
    ref_dur=len(ref)/expected_sr
    emu,emu_sr=read_wav_mono(wav_path)

    # Primary pitch/rate check: compare active-audio span in the emulator WAV
    # against the active-audio span in the reference.  This survives intentional
    # silence inside the clip better than a single full-envelope correlation.
    ref_env,ref_step=envelope(ref, expected_sr, 50.0)
    emu_env,emu_step=envelope(emu, emu_sr, 50.0)
    ref_segs=active_segments(ref_env, ref_step, min_start=0.0, min_len=1.0)
    emu_segs=active_segments(emu_env, emu_step, min_start=8.0, min_len=1.0)
    if not ref_segs or not emu_segs:
        raise SystemExit('could not find active audio segments for WAV/reference comparison')
    ref_span=ref_segs[-1][1]-ref_segs[0][0]
    # Choose the contiguous emulator segment group whose span is closest to the
    # reference.  This rejects BIOS/menu audio before the program starts.
    need=min(len(ref_segs), len(emu_segs))
    best_group=None
    for i in range(0, len(emu_segs)-need+1):
        grp=emu_segs[i:i+need]
        span=grp[-1][1]-grp[0][0]
        err=abs(span-ref_span)
        if best_group is None or err < best_group[0]:
            best_group=(err,grp,span)
    group=best_group[1]
    emu_span=best_group[2]
    active_scale=emu_span/ref_span if ref_span>0 else 1.0
    duration_error_pct=(active_scale - 1.0)*100.0
    effective_rate=expected_sr/active_scale if active_scale else 0.0

    # Secondary diagnostic: best envelope match.  This is reported but no longer
    # used as the sole pitch result because repeated/silent passages can produce
    # false matches.
    try:
        scale=best_envelope_scale(ref, expected_sr, emu, emu_sr, 8.0)
    except Exception:
        scale=dict(correlation=0.0, scale=active_scale, offset_sec=group[0][0], matched_duration_sec=emu_span)

    return dict(reference_duration_sec=ref_dur,
                reference_active_span_sec=ref_span,
                emulator_active_span_sec=emu_span,
                emulator_match_offset_sec=group[0][0],
                emulator_match_end_sec=group[-1][1],
                best_time_scale=active_scale,
                duration_error_pct=duration_error_pct,
                effective_sample_rate_hz=effective_rate,
                envelope_correlation=scale.get('correlation',0.0),
                envelope_best_time_scale=scale.get('scale',active_scale),
                envelope_best_offset_sec=scale.get('offset_sec',group[0][0]),
                emulator_wav_rate_hz=emu_sr,
                reference_active_segments=[dict(start_sec=a,end_sec=b,duration_sec=c) for a,b,c in ref_segs],
                active_segments=[dict(start_sec=a,end_sec=b,duration_sec=c) for a,b,c in emu_segs])

def load_stats(stats_dir:pathlib.Path) -> List[Tuple[str,Dict]]:
    out=[]
    for p in sorted(stats_dir.glob('stats_*.json')):
        try: st=json.loads(p.read_text())
        except Exception: continue
        out.append((p.name,st))
    return out

def av_report(stats:List[Tuple[str,Dict]], fps:float, sr:int) -> Dict:
    checkpoints=[]
    max_abs=0.0
    for name,st in stats:
        vf=float(st.get('g_video_frames_presented',0))
        ap=float(st.get('g_mp2psg10_read_pos',0))
        delta=ap/sr - vf/fps if fps>0 else 0.0
        max_abs=max(max_abs, abs(delta))
        checkpoints.append(dict(name=name, video_frames=int(vf), audio_samples=int(ap), av_delta_sec=delta,
                                psg_underflows=int(st.get('g_mp2psg10_underflows',0)),
                                video_underflows=int(st.get('g_scsi_dma_video_underflows',0)),
                                video_skipped=int(st.get('g_video_frames_skipped',0)),
                                done=int(st.get('g_done',0))))
    return dict(max_abs_av_delta_sec=max_abs, checkpoints=checkpoints)


def startup_report(stats:List[Tuple[str,Dict]]) -> Dict:
    chosen=None
    for name,st in stats:
        if int(st.get('g_audio_start_latch_frame',0)):
            chosen=(name,st)
            break
    if chosen is None and stats:
        chosen=stats[-1]
    if chosen is None:
        return dict(pass_=False, reason='no stats available')
    name,st=chosen
    return dict(
        stats=name,
        audio_start_latch_frame=int(st.get('g_audio_start_latch_frame',0)),
        audio_start_visible_fields=int(st.get('g_audio_start_visible_fields',0)),
        audio_start_ring_used=int(st.get('g_audio_start_ring_used',0)),
        audio_start_samples_emitted=int(st.get('g_audio_start_samples_emitted',0)),
        mp2_preroll_ring_at_exit=int(st.get('g_mp2_preroll_ring_at_exit',0)),
        mp2_preroll_guard_count=int(st.get('g_mp2_preroll_guard_count',0)),
        mp2_started=int(st.get('mp2_async',{}).get('started',0)),
    )

def read_rgb(path:pathlib.Path) -> np.ndarray:
    from PIL import Image
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.float32)

def source_frame_png(source:pathlib.Path, frame_index:int, fps:float, cache:pathlib.Path) -> pathlib.Path:
    cache.mkdir(parents=True, exist_ok=True)
    out=cache / f'source_{frame_index:05d}.png'
    if out.exists():
        return out
    vf=f'fps={fps:.8f},scale=256:240,select=eq(n\\,{frame_index})'
    subprocess.check_call(['ffmpeg','-hide_banner','-loglevel','error','-y','-i',str(source),'-vf',vf,'-frames:v','1',str(out)])
    if not out.exists():
        raise SystemExit(f'could not extract source frame {frame_index}')
    return out

def visual_report(frames_dir:pathlib.Path, source:pathlib.Path, stats:List[Tuple[str,Dict]], fps:float, frame_count:int) -> Dict:
    checks=[]
    failures=[]
    cache=frames_dir / '_source_cache'
    for name,st in stats:
        m=re.search(r'stats_(\d+)', name)
        if not m:
            continue
        tag=m.group(1)
        img=frames_dir / f'frame_{tag}.png'
        if not img.exists():
            continue
        vf=int(st.get('g_video_frames_presented',0))
        if vf <= 0:
            continue
        # When playback has completed, the app may have intentionally blanked the
        # display, so visual content checks are only meaningful before natural end.
        if int(st.get('g_done',0)):
            continue
        src_idx=max(0, min(frame_count-1, vf-1))
        ref_path=source_frame_png(source, src_idx, fps, cache)
        em=read_rgb(img)
        rf=read_rgb(ref_path)
        if em.shape != rf.shape:
            failures.append(f'{img.name}: shape {em.shape} != source {rf.shape}')
            continue
        gray=em.mean(axis=2)
        rgray=rf.mean(axis=2)
        black_total=float(np.mean(gray < 8.0))
        black_bottom=float(np.mean(gray[gray.shape[0]//2:,:] < 8.0))
        ref_black_bottom=float(np.mean(rgray[rgray.shape[0]//2:,:] < 8.0))
        mae=float(np.mean(np.abs(em-rf)))
        corr=float(np.corrcoef(em.ravel(), rf.ravel())[0,1]) if np.std(em)>1e-6 and np.std(rf)>1e-6 else 0.0
        # Rolling/corrupt RAINBOW failures usually appear as a black lower field
        # or a top/bottom split.  This catches the exact regression that slipped
        # through the previous package.
        ok=True
        reasons=[]
        if black_bottom > 0.70 and ref_black_bottom < 0.40:
            ok=False; reasons.append(f'bottom black band {black_bottom:.3f}')
        if (black_bottom - ref_black_bottom) > 0.45:
            ok=False; reasons.append(f'bottom black delta {black_bottom-ref_black_bottom:.3f}')
        if mae > 75.0:
            ok=False; reasons.append(f'MAE {mae:.2f}')
        if corr < 0.45:
            ok=False; reasons.append(f'corr {corr:.3f}')
        if not ok:
            failures.append(f'{img.name}: visual mismatch: ' + ', '.join(reasons))
        checks.append(dict(name=img.name, stats=name, source_frame=src_idx, video_frames_presented=vf,
                           black_total=black_total, black_bottom=black_bottom,
                           source_black_bottom=ref_black_bottom, mae=mae, correlation=corr,
                           pass_=ok, reasons=reasons))
    return dict(pass_=not failures, failures=failures, checks=checks)

def main():
    ap=argparse.ArgumentParser(description='Regression checks for PC-FX RAINBOW+MP2 validation artifacts.')
    ap.add_argument('--stream', required=True, type=pathlib.Path)
    ap.add_argument('--source', required=True, type=pathlib.Path)
    ap.add_argument('--wav', type=pathlib.Path)
    ap.add_argument('--stats-dir', required=True, type=pathlib.Path)
    ap.add_argument('--report', required=True, type=pathlib.Path)
    ap.add_argument('--frames-dir', type=pathlib.Path, help='Directory containing frame_N.png screenshots and stats_N.json for visual regression checks')
    ap.add_argument('--min-fps', type=float, default=15.0)
    ap.add_argument('--min-video-sectors', type=int, default=3)
    ap.add_argument('--max-av-drift-sec', type=float, default=0.35)
    ap.add_argument('--max-pitch-error-pct', type=float, default=0.50)
    ap.add_argument('--min-start-ring-samples', type=int, default=23040)
    ap.add_argument('--max-audio-start-visible-fields', type=int, default=1)
    args=ap.parse_args()

    stream=parse_pcfv(args.stream)
    stats=load_stats(args.stats_dir)
    sr=int(stream.get('sample_rate') or 16000)
    av=av_report(stats, stream['fps'], sr)
    startup=startup_report(stats)
    wav_cmp=compare_wav_to_reference(args.wav,args.source,sr) if args.wav else None
    visual=visual_report(args.frames_dir,args.source,stats,stream['fps'],stream['frames']) if args.frames_dir else None

    failures=[]
    if stream['fps'] < args.min_fps: failures.append(f'fps {stream["fps"]:.3f} < {args.min_fps:.3f}')
    if stream['max_video_sectors'] < args.min_video_sectors: failures.append(f'max_video_sectors {stream["max_video_sectors"]} < {args.min_video_sectors}')
    if av['max_abs_av_delta_sec'] > args.max_av_drift_sec: failures.append(f'max_abs_av_delta_sec {av["max_abs_av_delta_sec"]:.3f} > {args.max_av_drift_sec:.3f}')
    for cp in av['checkpoints']:
        if cp['psg_underflows']: failures.append(f'{cp["name"]}: PSG underflows {cp["psg_underflows"]}')
        if cp['video_underflows']: failures.append(f'{cp["name"]}: video underflows {cp["video_underflows"]}')
    if startup.get('audio_start_latch_frame',0) == 0:
        failures.append('audio never started according to startup counters')
    if startup.get('audio_start_visible_fields',999999) > args.max_audio_start_visible_fields:
        failures.append(f'audio starts after {startup.get("audio_start_visible_fields")} visible fields > {args.max_audio_start_visible_fields}')
    if startup.get('audio_start_ring_used',0) < args.min_start_ring_samples:
        failures.append(f'audio start ring {startup.get("audio_start_ring_used",0)} < {args.min_start_ring_samples} samples')
    if startup.get('mp2_preroll_ring_at_exit',0) < args.min_start_ring_samples:
        failures.append(f'MP2 hidden preroll ring {startup.get("mp2_preroll_ring_at_exit",0)} < {args.min_start_ring_samples} samples')
    if wav_cmp and abs(wav_cmp['duration_error_pct']) > args.max_pitch_error_pct:
        failures.append(f'pitch/duration error {wav_cmp["duration_error_pct"]:.3f}% > {args.max_pitch_error_pct:.3f}%')
    if visual and not visual['pass_']:
        failures.extend(visual['failures'])

    report=dict(pass_=not failures, failures=failures, stream=stream, startup_audio=startup, av_sync=av, pitch=wav_cmp, visual=visual,
                thresholds=dict(min_fps=args.min_fps,min_video_sectors=args.min_video_sectors,
                                max_av_drift_sec=args.max_av_drift_sec,max_pitch_error_pct=args.max_pitch_error_pct,
                                min_start_ring_samples=args.min_start_ring_samples,
                                max_audio_start_visible_fields=args.max_audio_start_visible_fields))
    args.report.parent.mkdir(parents=True,exist_ok=True)
    args.report.write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2))
    raise SystemExit(0 if not failures else 1)

if __name__=='__main__': main()
