#!/usr/bin/env python3
import argparse, struct, sys, zlib
SECTOR=2048
HDR=64
ENTRY=32
MAGIC=b'PCFV0001'
FLAG_MP2=0x0002
PCFV_WIDTH=256
PCFV_HEIGHT=240

def rd16(b,o): return struct.unpack_from('<H',b,o)[0]
def rd32(b,o): return struct.unpack_from('<I',b,o)[0]
def wr16(v): return struct.pack('<H',v)
def wr32(v): return struct.pack('<I',v)
def ceildiv(a,b): return (a+b-1)//b

def parse_pcfv(path):
    data=open(path,'rb').read()
    if data[:8] != MAGIC: raise SystemExit('input is not PCFV0001')
    w,h=rd16(data,8),rd16(data,10)
    fpsn, fpsd = rd16(data,12), rd16(data,14)
    fc=rd16(data,16)
    idx_bytes=rd32(data,20)
    data_start=rd32(data,24)
    maxvid=rd32(data,28)
    if w!=PCFV_WIDTH or h!=PCFV_HEIGHT: raise SystemExit('unexpected dimensions')
    if idx_bytes != fc*ENTRY: raise SystemExit('bad index size')
    entries=[]
    for i in range(fc):
        off=HDR+i*ENTRY
        vs=rd32(data,off+0); vsize=rd32(data,off+4); vsects=rd32(data,off+8); crc=rd32(data,off+12)
        if not vsects: raise SystemExit('zero video sectors')
        start=vs*SECTOR
        end=start+vsects*SECTOR
        frame=data[start:start+vsize]
        if len(frame)!=vsize: raise SystemExit('truncated video')
        entries.append(dict(video_size=vsize, video_sectors=vsects, video_crc32=crc, data=frame))
    return dict(fpsn=fpsn,fpsd=fpsd,frames=entries,max_video_sectors=maxvid)

def mux(video, mp2, out, lead_sectors, chunk_sectors):
    v=parse_pcfv(video)
    audio=open(mp2,'rb').read()
    fc=len(v['frames'])
    audio_total=len(audio)
    audio_sectors=ceildiv(audio_total,SECTOR) if audio_total else 0
    index_bytes=fc*ENTRY
    data_start=ceildiv(HDR+index_bytes,SECTOR)
    cur=data_start
    audio_cursor=0
    entries=[]
    # Sector-aligned chunks after frames. Initial lead goes after frame 0, then
    # remaining chunks are paced by the video timeline.
    bytes_per_frame = (audio_total / float(fc)) if fc else 0.0
    for i,fr in enumerate(v['frames']):
        e={}
        # Force every video frame to occupy max_video_sectors on disc.
        # This preserves the real compressed byte size in video_size, but gives
        # the PC-FX runtime a constant 4-sector stride so it can batch several
        # RAINBOW frames into one SCSI READ(10) directly into consecutive KRAM
        # frame buffers.  The extra sector padding is cheaper than per-frame
        # command latency and fixes audio running ahead of a starved video path.
        fixed_vsects = v['max_video_sectors']
        e['video_sector']=cur
        e['video_size']=fr['video_size']
        e['video_sectors']=fixed_vsects
        e['video_crc32']=fr['video_crc32']
        cur += fixed_vsects
        e['audio_sector']=0
        e['audio_sectors']=0
        e['audio_byte_offset']=0
        e['flags']=0
        if audio_cursor < audio_total:
            target = int((i+1)*bytes_per_frame)
            if i == 0:
                target += lead_sectors * SECTOR
            # Keep the disc roughly timeline ordered, but coalesce MP2 into
            # multi-sector chunks.  One-sector MP2 dribbles generate too many
            # SCSI READ(10) commands and starve RAINBOW on PC-FX.
            if target > audio_cursor:
                max_chunk = lead_sectors if i == 0 else chunk_sectors
                if max_chunk < 1: max_chunk = 1
                remaining_sectors = ceildiv(audio_total-audio_cursor, SECTOR)
                due_bytes = target - audio_cursor
                due_sectors = ceildiv(due_bytes, SECTOR)
                sectors_due = 0
                if i == 0:
                    sectors_due = max_chunk if remaining_sectors >= max_chunk else remaining_sectors
                elif due_sectors >= max_chunk:
                    sectors_due = max_chunk
                elif remaining_sectors <= max_chunk and target >= audio_total:
                    sectors_due = remaining_sectors
                if sectors_due:
                    e['audio_sector']=cur
                    e['audio_sectors']=sectors_due
                    e['audio_byte_offset']=audio_cursor
                    cur += sectors_due
                    audio_cursor += sectors_due*SECTOR
                    if audio_cursor > audio_total:
                        audio_cursor = audio_total
        entries.append(e)
    # Drain any remaining audio after the last frame, still referenced by last entry.
    if audio_cursor < audio_total:
        rem_secs=ceildiv(audio_total-audio_cursor,SECTOR)
        last=entries[-1]
        if last['audio_sectors']==0:
            last['audio_sector']=cur
            last['audio_byte_offset']=audio_cursor
            last['audio_sectors']=rem_secs
        else:
            # In practice this should not happen with the default pacing, but
            # append contiguous sectors and widen the last chunk if possible.
            if last['audio_sector'] + last['audio_sectors'] == cur:
                last['audio_sectors'] += rem_secs
            else:
                raise SystemExit('cannot represent extra final MP2 chunk in one PCFV entry')
        cur += rem_secs
        audio_cursor = audio_total
    with open(out,'wb') as f:
        f.write(MAGIC)
        f.write(wr16(PCFV_WIDTH)); f.write(wr16(PCFV_HEIGHT))
        f.write(wr16(v['fpsn'])); f.write(wr16(v['fpsd']))
        f.write(wr16(fc)); f.write(wr16(FLAG_MP2))
        f.write(wr32(index_bytes)); f.write(wr32(data_start))
        f.write(wr32(v['max_video_sectors']))
        f.write(wr32(audio_total)); f.write(wr32(audio_sectors))
        f.write(wr32(0)); f.write(wr32(chunk_sectors))
        f.write(wr32(0)); f.write(wr32(16000))
        f.write(wr32(0)); f.write(wr32(0))
        for e in entries:
            f.write(wr32(e['video_sector'])); f.write(wr32(e['video_size']))
            f.write(wr32(e['video_sectors'])); f.write(wr32(e['video_crc32']))
            f.write(wr32(e['audio_sector'])); f.write(wr32(e['audio_sectors']))
            f.write(wr32(e['audio_byte_offset'])); f.write(wr32(e['flags']))
        pos=HDR+index_bytes
        pad=data_start*SECTOR-pos
        if pad: f.write(b'\0'*pad)
        for i,fr in enumerate(v['frames']):
            f.write(fr['data'])
            fixed_vsects = v['max_video_sectors']
            f.write(b'\0'*(fixed_vsects*SECTOR-fr['video_size']))
            e=entries[i]
            if e['audio_sectors']:
                off=e['audio_byte_offset']; want=e['audio_sectors']*SECTOR
                part=audio[off:off+want]
                f.write(part)
                if len(part)<want: f.write(b'\0'*(want-len(part)))
    chunk_count=sum(1 for e in entries if e['audio_sectors'])
    print(f'Wrote {out}: {fc} RAINBOW frames, MP2={audio_total} bytes/{audio_sectors} sectors, chunks={chunk_count}, total={cur} sectors, flags=0x{FLAG_MP2:04x}', file=sys.stderr)

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--lead-sectors',type=int,default=4)
    ap.add_argument('--chunk-sectors',type=int,default=1)
    ap.add_argument('video_pcfv')
    ap.add_argument('audio_mp2')
    ap.add_argument('out_pcfv')
    a=ap.parse_args()
    mux(a.video_pcfv,a.audio_mp2,a.out_pcfv,a.lead_sectors,a.chunk_sectors)
if __name__=='__main__': main()
