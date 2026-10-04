# Original XMB interface sounds (48 kHz, 16-bit stereo WAV) for the PS4 launcher, synthesized:
# soft glassy "bell" tones (a few inharmonic partials with exponential decay) and short clicks.
# Themes may replace any of them with their own WAV files of the same names.
import math, os, struct, sys, wave

RATE = 48000
out_dir = sys.argv[1]

def bell(freq, dur, amp=0.5, decay=6.0, partials=((1, 1.0), (2.01, 0.35), (3.02, 0.12), (4.2, 0.05))):
    n = int(RATE * dur)
    out = []
    for i in range(n):
        t = i / RATE
        attack = min(1.0, t / 0.004)
        s = sum(a * math.sin(2 * math.pi * freq * m * t) * math.exp(-decay * m * 0.6 * t) for m, a in partials)
        out.append(amp * attack * math.exp(-decay * t) * s)
    return out

def mix(*parts):
    # parts: (start_seconds, samples)
    n = max(int(RATE * s) + len(p) for s, p in parts)
    out = [0.0] * n
    for s, p in parts:
        o = int(RATE * s)
        for i, v in enumerate(p):
            out[o + i] += v
    return out

def noise_click(dur=0.012, amp=0.25, tone=2400):
    n = int(RATE * dur)
    seed = 12345
    out = []
    for i in range(n):
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        r = (seed / 0x7FFFFFFF) * 2 - 1
        t = i / RATE
        out.append(amp * math.exp(-t * 400) * (0.4 * r + 0.6 * math.sin(2 * math.pi * tone * t)))
    return out

def write(name, samples, pan=0.0):
    peak = max(1e-9, max(abs(s) for s in samples))
    gain = min(1.0, 0.85 / peak)
    path = os.path.join(out_dir, name + ".wav")
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        frames = bytearray()
        for s in samples:
            v = int(max(-1.0, min(1.0, s * gain)) * 32767)
            frames += struct.pack("<hh", v, v)
        w.writeframes(bytes(frames))
    print("wrote", path, len(samples) / RATE, "s")

# Cursor up/down: tiny glassy tick.
write("move", [a * 0.55 for a in mix((0, noise_click(0.010, 0.3, 3200)), (0.0, bell(1760, 0.09, 0.25, 40)))])
# Category left/right: slightly lower, rounder.
write("category", mix((0, noise_click(0.012, 0.25, 1800)), (0.0, bell(1046.5, 0.16, 0.35, 22))))
# Confirm: two quick rising tones (E6, B6).
write("confirm", mix((0, bell(1318.5, 0.30, 0.4, 10)), (0.06, bell(1975.5, 0.40, 0.35, 8))))
# Back: two falling tones (B5, E5).
write("back", mix((0, bell(987.8, 0.22, 0.35, 14)), (0.05, bell(659.3, 0.30, 0.30, 12))))
# Launch a game: sparkling rising arpeggio (C6 E6 G6 C7).
write("launch", mix(*[(i * 0.07, bell(f, 0.9, 0.3, 4.5)) for i, f in enumerate((1046.5, 1318.5, 1568.0, 2093.0))]))
# Error / not available: low soft double thud.
write("error", mix((0, bell(220, 0.18, 0.5, 18, ((1, 1.0), (1.5, 0.3)))), (0.12, bell(196, 0.22, 0.5, 16, ((1, 1.0), (1.5, 0.3))))))
# Open a menu (Triangle / in-game L3+R3): soft whoosh-like chord.
write("menu", mix((0, bell(784.0, 0.35, 0.3, 9)), (0.02, bell(1174.7, 0.35, 0.25, 9)), (0.04, bell(1568.0, 0.35, 0.2, 9))))

# App start (the dolphin intro, 2.4 s): an airy swell while the dolphin leaps in (filtered noise
# rising), a soft splash and a warm chord as it lands (0.9 s), then a high sparkle for the title.
def swell(dur, amp=0.25):
    n = int(RATE * dur)
    seed, low, out = 777, 0.0, []
    for i in range(n):
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        r = (seed / 0x7FFFFFFF) * 2 - 1
        t = i / dur / RATE
        k = 0.01 + 0.12 * t  # the filter opens as it rises
        low += k * (r - low)
        env = math.sin(math.pi * min(1.0, t * 1.05)) ** 2
        out.append(amp * env * low * 3.0)
    return out

def splash(dur=0.35, amp=0.18):
    n = int(RATE * dur)
    seed, low, out = 4242, 0.0, []
    for i in range(n):
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        r = (seed / 0x7FFFFFFF) * 2 - 1
        low += 0.35 * (r - low)
        t = i / RATE
        out.append(amp * min(1.0, t / 0.01) * math.exp(-t * 12) * (r - low))
    return out

write("intro", mix((0.0, swell(0.95)),
                   (0.88, splash()),
                   (0.90, bell(392.0, 1.5, 0.30, 2.2)), (0.93, bell(493.9, 1.4, 0.24, 2.4)),
                   (0.96, bell(587.3, 1.3, 0.22, 2.6)), (0.99, bell(784.0, 1.2, 0.18, 2.8)),
                   *[(1.15 + i * 0.06, bell(f, 0.6, 0.10, 6)) for i, f in enumerate((1568.0, 1975.5, 2349.3, 3136.0))]))
