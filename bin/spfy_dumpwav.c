// cl spfy_dumpwav.c /Fe:spfy_dumpwav.exe
// usage: spfy_dumpwav.exe "text to speak" out.wav

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "swi_min.h" // Assumed to contain SWIttsPort, SWITTS_INVALID_PORT, etc.

// crude wave helper
static void write_wav_header(FILE *f, uint32_t sampleRate, uint16_t bits, uint16_t chans, uint32_t dataBytes) {
    uint32_t riffSize = 36 + dataBytes;
    fwrite("RIFF",1,4,f); fwrite(&riffSize,4,1,f); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f); uint32_t fmtSize=16; fwrite(&fmtSize,4,1,f);
    uint16_t audioFmt=1; fwrite(&audioFmt,2,1,f);
    fwrite(&chans,2,1,f);
    fwrite(&sampleRate,4,1,f);
    uint32_t byteRate = sampleRate * chans * (bits/8); fwrite(&byteRate,4,1,f);
    uint16_t blockAlign = chans * (bits/8); fwrite(&blockAlign,2,1,f);
    fwrite(&bits,2,1,f);
    fwrite("data",1,4,f); fwrite(&dataBytes,4,1,f);
}

typedef struct {
    FILE *out;
    volatile LONG gotAudio;   // becomes 1 after first audio packet
    volatile LONG done;       // becomes 1 when we think speak is finished
    uint32_t bytesWritten;
} Ctx;

static SWIttsResult SWIAPI cb(SWIttsPort port, int status, void *data, void *user) {
    Ctx *ctx = (Ctx*)user;

    // Check for log/diag message
    if (port == (SWIttsPort)-1) {
        if (data) {
            fwprintf(stderr, L"DEBUG (status=%d, data_as_str): %hs\n", status, (char*)data);
        }
        return 0; // Acknowledge log message
    }

    // Original audio/status handling
    if (data) {
        // Heuristic: assume this is audio if it looks like a packet and numBytes is sane.
        SWIttsAudioPacket *p = (SWIttsAudioPacket*)data;

        if (p->samples && p->numBytes && p->numBytes < (1u<<26)) {
            // network byte order -> little endian for 16-bit PCM
            uint8_t *buf = (uint8_t*)p->samples;
            for (unsigned i=0;i+1<p->numBytes;i+=2) {
                uint8_t t=buf[i]; buf[i]=buf[i+1]; buf[i+1]=t;
            }
            fwrite(buf,1,p->numBytes,ctx->out);
            ctx->bytesWritten += p->numBytes;
            InterlockedExchange(&ctx->gotAudio, 1);
            return 0;
        } else {
            // This is a non-audio status packet. We can just log and ignore it.
            fwprintf(stderr, L"INFO: Received non-audio/non-log packet on port %d (status=%d).\n", (int)port, status);
        }

    } else {
        // NULL data cases include start/end/stopped/portclosed/errors.
        // Treat a NULL after we’ve seen audio as “probably end”.
        if (ctx->gotAudio) {
            InterlockedExchange(&ctx->done, 1);
        } else {
            fwprintf(stderr, L"INFO: Received NULL data on port %d (status=%d) before audio. Done=%d\n",
                (int)port, status, (int)ctx->done);

            if (status != 0) { // Assuming non-zero status is an error
                fwprintf(stderr, L"  -> Non-zero status (%d) with NULL data. Assuming ERROR, setting done=1.\n", status);
                InterlockedExchange(&ctx->done, 1);
            }
        }
    }
    return 0;
}

int wmain(int argc, wchar_t **wargv) {
    if (argc < 3) {
        fwprintf(stderr, L"usage: %ls \"text to speak\" out.wav\n", wargv[0]);
        return 2;
    }

    // Convert wide text to UTF-8
    const wchar_t *wtext = wargv[1];
    int need = WideCharToMultiByte(CP_UTF8,0,wtext,-1,NULL,0,NULL,NULL);
    char *utf8 = (char*)malloc(need);
    WideCharToMultiByte(CP_UTF8,0,wtext,-1,utf8,need,NULL,NULL);

    // Open output
    FILE *f = _wfopen(wargv[2], L"wb+");
    if (!f) { fprintf(stderr, "failed to open output\n"); return 3; }

    // --- FIX: Write 16kHz WAV header ---
    write_wav_header(f, 16000, 16, 1, 0);

    // Load the client DLL
    SWIttsAPI api;
    if (!LoadSWItts(&api, L".\\SWItts.dll")) {
        fprintf(stderr, "Failed to load SWItts.dll from .\n");
        return 4;
    }

    Ctx ctx = {0};
    ctx.out = f;

    // --- FIX: Pass 'cb' directly, not '&cb' ---
    if (api.Init(cb, &ctx) != 0) { fprintf(stderr, "SWIttsInit failed\n"); return 5; }

    // Open a port
    SWIttsPort port = SWITTS_INVALID_PORT;
    const char *params = "hostname=127.0.0.1;hostport=5555"; // adjust if your server differs

    // --- FIX: Pass 'cb' directly, not '&cb' ---
    if (api.OpenPortEx(&port, params, NULL, cb, &ctx) != 0 || port == SWITTS_INVALID_PORT) {
        fprintf(stderr, "SWIttsOpenPortEx failed\n");
        // --- FIX: Pass 'cb' directly, not '&cb' ---
        api.Term(cb, &ctx);
        return 6;
    }

    fwprintf(stderr, L"INFO: Port opened: %d\n", (int)port);

    // --- FIX: Request 16kHz, which the server supports ---
    if (api.SetParameter(port, "tts.audioformat.mimetype", "audio/L16;rate=16000") != 0) {
        fprintf(stderr, "SetParameter(tts.audioformat.mimetype) failed\n");
    }

    // --- Removed the noisy log level parameters ---

    // Speak (UTF-8 text) – content type indicates charset
    const unsigned char *bytes = (const unsigned char*)utf8;
    unsigned len = (unsigned)strlen(utf8);
    fwprintf(stderr, L"INFO: Calling Speak...\n");
    // --- FIX: Use 'utf-8' ---
    if (api.Speak(port, bytes, len, "text/plain;charset=utf-8") != 0) {
        fprintf(stderr, "SWIttsSpeak failed\n");
    }

    // Pump messages while callback writes audio; trivial sleep loop
    while (!ctx.done) Sleep(10);

    // --- FIX: Add 'stderr,' to fwprintf ---
    fwprintf(stderr, L"INFO: Loop finished. Closing port.\n");

    // Close port and term
    api.ClosePort(port);
    // --- FIX: Pass 'cb' directly, not '&cb' ---
    api.Term(cb, &ctx);

    // --- FIX for cut-off audio ---
    // Explicitly flush all buffered I/O to disk *before*
    // we start patching the header. This wins the race.
    fflush(f);

    // Patch WAV sizes
    long end = ftell(f);
    uint32_t dataBytes = ctx.bytesWritten;
    fseek(f, 4, SEEK_SET);  uint32_t riffSize = 36 + dataBytes; fwrite(&riffSize,4,1,f);
    fseek(f, 40, SEEK_SET); fwrite(&dataBytes,4,1,f);
    fclose(f);

    free(utf8);
    fprintf(stderr, "Wrote %u bytes of audio to output\n", dataBytes);
    return 0;
}
