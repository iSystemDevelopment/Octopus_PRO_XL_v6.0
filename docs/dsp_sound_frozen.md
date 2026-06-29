# DSP / sound engine policy (FROZEN)

**Status:** Normative — the shipped sound is **validated and locked**.  
**Scope:** `audio.cpp`, `effect.cpp` / `effect.h`, `harp.cpp`, `groovebox.cpp` (seq + drum synth), `arp.h`, wavetable/drum voice paths.  
**Related:** `docs/task_schedule.md` (scheduler only — not DSP refactors), `code_info.h` §8.

---

## 1. Policy

**Do not optimize the audio, effect, or synth engines.**

The current sound — depth, envelopes, LFO behaviour, FX colour, note on/off, accordion/poly stress patches — is the product. Task-priority fixes proved that **faster delivery of the same DSP** improves perceived quality; **changing the DSP itself** is out of scope unless a specific bug is proven.

| Allowed | Forbidden |
|---------|-----------|
| **Recalculated touches** — fix a coefficient, scale, or table entry that is **wrong vs intent** | “Optimization” passes (SIMD, loop fusion, `-O` driven rewrites, buffer pooling) |
| Restore **documented** behaviour that regressed (click, stuck note, wrong ms attack) | New algorithms, alternate filters, different envelope shapes |
| Wavetable / preset **content** edits (musical, not structural) | FX topology changes (insert order, aux architecture, voice cap logic) |
| Bug fix with **A/B listen** on reference patch | Adaptive load-shedding tier changes without hardware re-validation |
| Comments / docs only | Refactors “for clarity” in IRAM hot paths |

**Recalculated touch** means: change one derived constant (e.g. `attackSamples = f(ms, rate)`), one LUT sample, one send ratio — with before/after listen on a **fixed reference patch**, not a rewrite of the engine.

---

## 2. Frozen modules (do not refactor)

| Module | Hot path | Notes |
|--------|----------|-------|
| `audio_synthesis_task` | I2S block loop | Shedding tiers exist — do not retune without §4 checklist |
| `fx_process_multi_buf_safe` | IRAM, per-sample | Single-pass; no staging buffers |
| `sequencer_render_block` | Sample-locked clock | µs accumulator — tempo authority |
| `harp_synth_fill_buf` / voices | Harp | 5 ms attack / 10 ms release minimums |
| `gb_seq_fill_buf` / seq voices | Seq synth | Same envelope discipline |
| `drum_fill_buf` | Drums | One-shot decay envelopes |
| Insert A/B + shared aux | `effect.cpp` | 3-instrument groovebox pipeline |

---

## 3. Reference validation patch (user-signed)

Re-listen after **any** DSP-touch change:

- 100% matrix coverage  
- BPM 80  
- LFO-pitch ~8 Hz  
- Glass pad + short DSR (~0.23 s shot, DSR 0 / 0 / 0.15)  
- Accordion-style riff — full, deep, **no clicks**, no zipper on param moves  

If it fails, **revert the touch** — do not “optimize further.”

---

## 4. What already improved sound (keep separate)

Scheduler / I/O priority (`docs/task_schedule.md`) — **not** DSP:

- NvsWorker Core 0 @ 16  
- MidiUsbRx @ 22, SeqSysexOut @ 23  

That made param echoes arrive on time → deeper, cleaner sound **without** touching synth code. Future performance work stays in **scheduling and I/O**, not engine rewrites.

---

## 5. Explicit rejections

- Rewriting `safe_sinf` / denormal flush “for speed”  
- Voice-steal policy changes “for CPU”  
- Merging harp + seq synthesis TUs  
- Replacing adaptive shedding with hard caps or vice versa  
- “Modernizing” FX with new reverb/delay structures  
- App-side or firmware “quality modes” that alter DSP topology  

---

*Frozen 2026-06-25 — sound is the product; recalculate, don’t reinvent.*
