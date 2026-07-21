# Speechify Engine Speed Fix

## The Problem

Synthesizing a 200-word paragraph (~73 seconds of audio) takes **41 seconds** with the stock Speechify engine. Every other TTS engine of similar vintage (AT&T Natural Voices, L&H, Nuance) completes equivalent tasks in under a second. Something is drastically wrong.

## Investigation

### Profiling the Pipeline

Using Frida to instrument the key DLL exports in `Speechify.exe`:

| Function | DLL | Time | Calls |
|----------|-----|------|-------|
| `SWIttsUSelUnitSelection` | SWIttsUSel.dll | 78ms | 28 |
| `SWIttsWsolaConcat` | SWIttsWsola.dll | 53ms | 28 |
| `SWIttsSpeak` | SWIttsEngine.dll | 3ms | 1 |

**Total actual computation: 136ms** out of 41,000ms wall time. The engine is fast -- 99.7% of the time is unaccounted for.

### Finding the Bottleneck

Hooking `kernel32.Sleep` in `Speechify.exe` revealed:

```
Sleep: 35,272ms requested (231 calls)
```

The engine deliberately sleeps for **35 seconds** during synthesis. The sleep durations increase progressively (67ms, 320ms, 574ms, 827ms, 1081ms...) -- a classic **realtime playback throttle**. The engine calculates how far ahead of realtime it is and sleeps to match, preventing audio buffer overflow during live playback.

This is completely unnecessary for file output (which is what `spfy_dumpwav.exe` does).

### Client-Side Overhead

The remaining ~5 seconds after removing server-side Sleep was traced to `spfy_dumpwav.exe` itself:

```c
// Original code (line 519 of spfy_dumpwav.c):
while (!ctx.done) Sleep(10);
```

A polling loop with 10ms sleep. Fixed by replacing with a proper Win32 Event (`WaitForSingleObject`).

### Combined Results

| Configuration | Time | Speedup |
|--------------|------|---------|
| Stock (no fix) | 41.2s | 1x |
| Server DLL patch only | 5.5s | 7.5x |
| DLL patch + client Event fix | 2.9s | 14.2x |

The ~2.9s floor with both fixes is a fixed cost per request, not per word.

> **Superseded 2026-07-20 — the floor was NOT IPC.** This section previously
> attributed the floor to "each invocation establishes a new TCP connection
> to `Speechify.exe`". Direct phase timing disproves that: connecting and
> opening a port costs **61 ms**, and synthesis completes at **88 ms**.
> The entire remaining ~4.6 s is `SWIttsTerm()` plus `SWItts.dll`'s
> `DLL_PROCESS_DETACH` — pure client-side teardown, after the server port
> has already been released. See "Teardown, not IPC" below.

## The Fix

### Server-Side: SWIttsEngine.dll Binary Patch

**Location:** File offset `0x123DA` in SWIttsEngine.dll (RVA `0x123E1` return address)

**Call site identification:** Using Frida's `Interceptor.attach` on `kernel32.Sleep` with return address tracking, all 231 throttle Sleep calls originate from a single `CALL` instruction in SWIttsEngine.dll.

**Original bytes (7 bytes at 0x123DA):**
```
56              PUSH ESI          ; sleep duration in ms (throttle value)
FF 15 24 F0 B1 06  CALL [06B1F024]   ; kernel32.Sleep via IAT
```

**Patched bytes:**
```
90 90 90 90 90 90 90  ; 7x NOP (skip push + call entirely)
```

This is safe because:
- `Sleep` returns void (no return value to consume)
- The PUSH/CALL pair is self-contained (stack balanced by NOP-ing both)
- No other code depends on the Sleep side effect

**Applying:** Run `python bin/patch_speed.py` (stops server first). The script verifies the original bytes before patching and creates `SWIttsEngine_orig.dll` as a backup.

### Client-Side: spfy_dumpwav.c Source Fix

```c
// Before:
while (!ctx.done) Sleep(10);

// After:
WaitForSingleObject(ctx.doneEvent, 60000);
CloseHandle(ctx.doneEvent);
```

The `doneEvent` is created during init (`CreateEvent`) and signaled in the audio callback when synthesis completes (`SetEvent`). This eliminates polling entirely.

## Architecture Context

The Speechify engine uses a client-server architecture:

```
spfy_dumpwav.exe  --TCP:5555-->  Speechify.exe
  (client)                         (server)
                                     |
                            SWIttsEngine.dll  (orchestration)
                            SWIttsFe-en-US.dll (text analysis)
                            SWIttsUSel.dll     (unit selection, 78ms)
                            SWIttsWsola.dll    (WSOLA synthesis, 53ms)
```

`SWIttsSpeak` is asynchronous -- it returns in 3ms, dispatching synthesis to a worker thread. The worker thread produces audio chunks and calls back to the client via the TCP connection. Between chunks, the worker calls `Sleep(throttle_ms)` where `throttle_ms` is calculated to match the realtime audio duration produced so far.

This design made sense for SpeechWorks' original use case (streaming audio to a telephony system in 2003), but is counterproductive for modern batch synthesis. This change does not affect streaming use cases (i.e. Balabolka live playback). Synthesis works just fine there, too.

## Discovery Timeline

1. Baseline measurement: 41.2s for 200 words (0.56x realtime)
2. Frida pipeline profiling: USel+WSOLA = 136ms total (0.3% of wall time)
3. Sleep tracking: 35.3s in 231 Sleep calls (85% of wall time)
4. Call site identification: single CALL at SWIttsEngine.dll+0x123DA
5. Sleep duration analysis: progressive throttle (67, 320, 574, 827, 1081ms...)
6. Client polling discovery: `Sleep(10)` loop in spfy_dumpwav.c
7. Binary patch: 7-byte NOP at file offset 0x123DA
8. Client fix: Event-based wait replaces polling loop
9. Final result: 41.2s -> 5.5s (DLL only) or 2.9s (both fixes)

---

## Teardown, not IPC (2026-07-20)

The "~2.9s IPC floor" above was a misattribution. Phase-timing a single
`spfy_dumpwav.exe` run against the live server shows where the time
actually goes:

| phase | elapsed |
|---|---|
| `SWIttsInit` + `SWIttsOpenPortEx` (TCP connect + session) | 61 ms |
| `SWIttsSpeak` + audio fully received | **88 ms** |
| `SWIttsClosePort` | 0 ms |
| `SWIttsTerm` | **4,585 ms** |

Connecting is cheap and synthesis is cheap. **98% of every invocation was
client-side library teardown**, paid after the server had already
released the port. Measured mean over the 235-phrase corpus before the
fix: **4,683 ms per phrase, with a standard deviation of ~10 ms and no
correlation to text length** — 2 characters cost the same as 44, which is
the signature of a fixed cost rather than work.

### Two fixes

1. **Skip `SWIttsTerm()`** (`--graceful-term` restores it). `ClosePort`
   has already freed the server-side port; `Term` is client-side cleanup
   the OS is about to do anyway.
2. **Exit via `TerminateProcess`, not `return`/`exit()`.** Skipping
   `Term` alone changes nothing measurable: `SWItts.dll` performs the
   same ~4.6 s of cleanup from its `DLL_PROCESS_DETACH` handler, and
   `ExitProcess` (which `return` from `wmain` and `exit()`/`_exit()` all
   route through) still notifies loaded DLLs. Only `TerminateProcess`
   bypasses detach. Safe because every file the tool owns is `fclose`d
   and the WAV is finalized on disk before the call.

### `--batch` mode

For corpus work the per-process cost can be amortized entirely. `--batch
<outdir>` reads `<id>\t<text>` lines from stdin, synthesizes each on ONE
port, and prints `DONE <id>` after each. Reading a line at a time keeps
it lock-step, so a Frida capture can drain its hook ring between
utterances.

### Results

| mode | per phrase | 235-phrase corpus |
|---|---|---|
| stock (before) | 4,683 ms | 18.3 min |
| single-shot, fast exit | 92 ms | 21.6 s |
| `--batch` (one port) | **34 ms** | **8.1 s** |

**51x** on single-shot, **138x** on batch.

### Correctness checks

- Output is **byte-identical** (SHA-256) to the pre-change binary, both
  for single-shot and for batch — so reusing one port across a whole
  corpus does not perturb synthesis. That matters for oracle capture:
  the existing Tom traces were taken one-process-per-phrase.
- **120-invocation soak, 0 failures.** `tts.server.numPorts` is 100, so
  a leaked port per run would have started failing around iteration 101.
  It does not: the server reaps the port when the TCP connection dies
  with the process.

Rebuild with `bin\build_dumpwav.bat` (MSVC x86 — must be 32-bit, it
`LoadLibrary`s the 32-bit `SWItts.dll`).

