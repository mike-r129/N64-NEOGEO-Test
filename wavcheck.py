#!/usr/bin/env python3
# Stdlib-only WAV glitch analyzer for N64 NeoGeo audio validation.
# No numpy (not installed in WSL). Reports:
#   - RMS per 0.1s window + how many windows are silent vs loud (continuity)
#   - zero-run detection (gaps): longest run of all-zero stereo frames
#   - discontinuity count (large sample-to-sample jumps = clicks/glitches)
#   - autocorrelation pitch estimate on the loudest 1s window (tonal music check)
import sys, wave, audioop, struct

def main(path):
    w = wave.open(path, 'rb')
    ch, width, fr, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    raw = w.readframes(n); w.close()
    print(f"file={path}")
    print(f"  channels={ch} width={width} rate={fr} frames={n} dur={n/fr:.2f}s")
    if n == 0:
        print("  EMPTY: no audio frames captured."); return
    mono = audioop.tomono(raw, width, 0.5, 0.5) if ch == 2 else raw
    # per-0.1s RMS
    win = max(1, fr // 10)
    rmss = []
    for i in range(0, len(mono) - (len(mono) % (win*width)), win*width):
        rmss.append(audioop.rms(mono[i:i+win*width], width))
    if not rmss: rmss=[audioop.rms(mono,width)]
    peak = max(rmss); avg = sum(rmss)/len(rmss)
    silent = sum(1 for r in rmss if r < 30)
    loud   = sum(1 for r in rmss if r > peak*0.25) if peak else 0
    print(f"  RMS windows(0.1s): n={len(rmss)} avg={avg:.0f} peak={peak} "
          f"silent={silent}({100*silent/len(rmss):.0f}%) active={loud}({100*loud/len(rmss):.0f}%)")
    # zero-run (gap) detection on mono samples
    fmt = {1:'b',2:'h',4:'i'}[width]
    samples = struct.unpack(f"<{len(mono)//width}{fmt}", mono)
    longest_zero = cur = 0
    for s in samples:
        if s == 0:
            cur += 1; longest_zero = max(longest_zero, cur)
        else: cur = 0
    print(f"  longest zero-run: {longest_zero} samples = {1000*longest_zero/fr:.1f} ms")
    # discontinuity: count adjacent jumps > 25% full-scale (clicks)
    fullscale = float(1 << (8*width - 1))
    thr = 0.25 * fullscale
    jumps = sum(1 for a, b in zip(samples, samples[1:]) if abs(b - a) > thr)
    print(f"  large jumps(>25%FS): {jumps} ({1000.0*jumps/len(samples):.2f} per 1000 samples)")
    # autocorrelation pitch on loudest 1s window
    if peak:
        li = max(range(len(rmss)), key=lambda i: rmss[i])
        start = li * win
        seg = samples[start:start + fr]
        if len(seg) > 200:
            seg = [float(x) for x in seg]
            mean = sum(seg)/len(seg)
            seg = [x-mean for x in seg]
            e0 = sum(x*x for x in seg) or 1.0
            best_r, best_lag = 0.0, 0
            for lag in range(40, min(1000, len(seg)//2)):  # ~44Hz..1100Hz
                r = sum(seg[i]*seg[i+lag] for i in range(0, len(seg)-lag, 4))
                r /= e0
                if r > best_r: best_r, best_lag = r, lag
            if best_lag:
                print(f"  autocorr: peak={best_r:.2f} @ lag={best_lag} -> ~{fr/best_lag:.0f} Hz (tonal if >0.3)")

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "out.wav")
