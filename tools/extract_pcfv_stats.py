#!/usr/bin/env python3
import re, struct, sys, json
map_path, ram_path = sys.argv[1], sys.argv[2]
text=open(map_path).read().splitlines()
syms={}
for i,line in enumerate(text):
    gm=re.match(r'\s*0x([0-9a-fA-F]+)\s+([A-Za-z_][A-Za-z0-9_]*)\s*$', line)
    if gm:
        syms.setdefault(gm.group(2),(int(gm.group(1),16),4))
    m=re.match(r'\.bss\.([A-Za-z0-9_]+)\s*$', line.strip())
    if m and i+1<len(text):
        mm=re.search(r'0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)', text[i+1])
        if mm:
            syms[m.group(1)]=(int(mm.group(1),16), int(mm.group(2),16))
    m=re.match(r'\.bss\.([A-Za-z0-9_]+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)', line.strip())
    if m:
        syms[m.group(1)]=(int(m.group(2),16), int(m.group(3),16))
ram=open(ram_path,'rb').read()
def rd(addr,n):
    return ram[addr:addr+n]
def u8(name):
    a,s=syms[name]; return rd(a,1)[0]
def u16(name):
    a,s=syms[name]; return struct.unpack_from('<H', ram, a)[0]
def u32(name):
    a,s=syms[name]; return struct.unpack_from('<I', ram, a)[0]
keys32=['g_video_frames_presented','g_video_frames_skipped','g_video_frames_held','g_video_frames_dropped_stale','g_vblank_latched_frames','g_midfield_latched_frames','g_scsi_dma_video_underflows','g_scsi_dma_late_frames','g_scsi_dma_errors','g_scsi_dma_completed','g_scsi_dma_started','g_video_ready_highwater','g_audio_start_latch_frame','g_audio_start_visible_fields','g_audio_start_ring_used','g_audio_start_samples_emitted','g_mp2_preroll_ring_at_exit','g_mp2_preroll_guard_count','g_rainbow_visible_fields','g_mp2_sync_pauses','g_mp2_sync_resumes','g_audio_clock_samples',
        'g_adpcm_underruns','g_adpcm_refills','g_adpcm_boundaries','g_adpcm_parity_errors','g_adpcm_clock_error_max',
        'g_adpcm_play_block','g_adpcm_requests']
keys16=['g_frame_count','g_start_frame','g_next_display_frame','g_next_load_frame','g_fields_per_frame','g_field_counter','g_audio_chunk_count','g_next_audio_chunk']
keys8=['g_done','g_abort','g_header_ready','g_rainbow_visible','g_audio_codec','g_audio_active','g_mp2_enabled',
       'g_adpcm_enabled','g_adpcm_started','g_adpcm_playing','g_adpcm_finished','g_paused']
out={}
for k in keys32:
    if k in syms: out[k]=u32(k)
for k in keys16:
    if k in syms: out[k]=u16(k)
for k in keys8:
    if k in syms: out[k]=u8(k)
# psg globals
for k in ['g_mp2psg10_read_pos','g_mp2psg10_write_pos','g_mp2psg10_playing','g_mp2psg10_underflows','g_mp2psg10_decode_done','g_mp2psg10_timer_period_base','g_mp2psg10_timer_period_frac','g_mp2psg10_timer_period_accum']:
    if k in syms: out[k]=u32(k)
# mp2 async
if 'g_mp2_async' in syms:
    base,_=syms['g_mp2_async']
    off=0x4008
    fields=['src','pos','end','size','sample_rate','channels','bytes_loaded','bytes_consumed','frames_decoded','samples_written','last_frame_bytes','max_ring_used','error_code','error_offset']
    mp={}
    for idx,f in enumerate(fields):
        mp[f]=struct.unpack_from('<I', ram, base+off+idx*4)[0]
    flags_off=base+0x4040
    for idx,f in enumerate(['opened','started','done','stream_mode','input_eof','timer_inited']):
        mp[f]=ram[flags_off+idx]
    out['mp2_async']=mp
print(json.dumps(out,indent=2))
