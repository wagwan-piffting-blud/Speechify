// spfy GUI - Tauri shell.
//
// The engine is NEVER linked into this process. Every synth spawns
// `spfy_synth`, exactly the way `spfy_sapi64.dll` already does for 64-bit
// SAPI clients. Two reasons, both load-bearing:
//
//   * The engine is ONE SYNTH PER PROCESS. `spfy/src/cli/spfy_synth.c` carries
//     76 file-scope statics and `src/host_emu` keeps global guest state for the
//     emulated front end. Two concurrent synths in one process is not a thing
//     that works.
//   * Byte-exactness. The project's claim is that the CLI, SAPI, the WASM build
//     and CI all produce the same samples; a GUI that grew its own synthesis
//     path would quietly stop being covered by that.
//
// Measured cost (2026-08-23, warm cache, whole process - voice load, FE boot,
// synth, WAV write): 70-114 ms for tom, 94-140 ms for the 152 MB crstom, 138 ms
// for a full bulletin sentence. Under the ~200 ms where a UI starts to feel
// slow.

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use std::path::PathBuf;
use std::process::Command;

#[cfg(windows)]
use std::os::windows::process::CommandExt;

/// Keep the console window from flashing on every utterance.
///
/// ⚠ Without this the app spawns a visible conhost several times a minute.
/// It is also an ACCESSIBILITY bug, not just an eyesore: the flash steals
/// focus, and a screen reader announces the console each time.
#[cfg(windows)]
const CREATE_NO_WINDOW: u32 = 0x0800_0000;

fn no_window(cmd: &mut Command) {
    #[cfg(windows)]
    cmd.creation_flags(CREATE_NO_WINDOW);
    #[cfg(not(windows))]
    let _ = cmd;
}

/// Locate `spfy_synth`.
///
/// Order is deliberate:
///   1. `$SPFY_SYNTH` - how a dev checkout points at a build tree.
///   2. Beside this executable - how the shipped install works. The Inno
///      script puts `spfy_synth.exe` in `{app}` and this binary lands there
///      too, so "next to me" is the normal case on Windows.
///   3. Bare name, letting the OS search PATH.
fn synth_exe() -> PathBuf {
    if let Some(p) = std::env::var_os("SPFY_SYNTH") {
        let p = PathBuf::from(p);
        if p.is_file() {
            return p;
        }
    }
    let name = if cfg!(windows) { "spfy_synth.exe" } else { "spfy_synth" };
    if let Ok(me) = std::env::current_exe() {
        if let Some(dir) = me.parent() {
            let cand = dir.join(name);
            if cand.is_file() {
                return cand;
            }
        }
    }
    PathBuf::from(name)
}

fn spawn_failed(e: std::io::Error) -> String {
    format!(
        "Could not run {}: {e}.\n\nSet SPFY_SYNTH to its full path, or put it \
         beside this application.",
        synth_exe().display()
    )
}

/// Every voice the engine's own search path can see.
///
/// Returns whatever `--list-voices --json` produced, parsed. An EMPTY voices
/// array is a normal answer, not an error: it is the first thing a new user
/// hits, because voices are 66-222 MB and are not bundled. The UI says so and
/// hands over `search_path`.
fn voices() -> Result<serde_json::Value, String> {
    let exe = synth_exe();
    let mut cmd = Command::new(&exe);
    cmd.args(["--list-voices", "--json"]);
    no_window(&mut cmd);
    let out = cmd.output().map_err(spawn_failed)?;

    let text = String::from_utf8_lossy(&out.stdout);
    serde_json::from_str(&text).map_err(|e| {
        format!(
            "{} --list-voices --json did not return JSON ({e}).\nstderr: {}",
            exe.display(),
            String::from_utf8_lossy(&out.stderr).trim()
        )
    })
}

/// Synthesise `text` in `voice` and hand back a complete WAV.
///
/// The bytes come back as a binary IPC response, so the browser side can pass
/// them straight to `decodeAudioData` - no PCM unpacking in JS, and no
/// 100k-element JSON array on the wire.
///
/// ⚠ Text goes to the engine through a FILE, never as an argv string. On
/// Windows argv arrives in the ANSI code page, so anything non-ASCII - an
/// accent, an SSML `<phoneme ph="h\u{259}\u{2c8}lo\u{28a}">` - loses every
/// byte above 0x7F before main() sees it, silently and with no error.
fn synth_wav(voice: &str, text: &str) -> Result<Vec<u8>, String> {
    if voice.trim().is_empty() {
        return Err("No voice selected.".into());
    }
    if text.trim().is_empty() {
        return Err("Nothing to speak.".into());
    }

    let dir = std::env::temp_dir();
    let stamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    let txt = dir.join(format!("spfy_gui_{stamp}.txt"));
    let wav = dir.join(format!("spfy_gui_{stamp}.wav"));

    std::fs::write(&txt, text.as_bytes())
        .map_err(|e| format!("Could not write the input file {}: {e}", txt.display()))?;

    let exe = synth_exe();
    let mut cmd = Command::new(&exe);
    // --silent so nothing but a real error reaches stderr; anything we find
    // there is worth showing the user verbatim.
    cmd.arg("--silent")
        .arg("-f")
        .arg(&txt)
        .arg(voice)
        .arg(&wav);
    no_window(&mut cmd);
    let out = cmd.output().map_err(spawn_failed);

    let _ = std::fs::remove_file(&txt);
    let out = out?;

    if !out.status.success() {
        let _ = std::fs::remove_file(&wav);
        let err = String::from_utf8_lossy(&out.stderr);
        let err = err.trim();
        return Err(if err.is_empty() {
            format!("{} exited with {}", exe.display(), out.status)
        } else {
            err.to_string()
        });
    }

    let bytes = std::fs::read(&wav)
        .map_err(|e| format!("Synthesis reported success but {} is unreadable: {e}", wav.display()));
    let _ = std::fs::remove_file(&wav);
    bytes
}

#[tauri::command]
fn list_voices() -> Result<serde_json::Value, String> {
    voices()
}

/// Voices the release offers, and which are already here.
///
/// The GUI does not download anything itself: `spfy_synth` gained
/// `--list-available` and `--install-voice`, so this is a subprocess call like
/// every other. One implementation of catalog-fetch, checksum and unzip,
/// shared by the CLI and the GUI, rather than a second one in Rust that could
/// disagree with it.
#[tauri::command]
fn list_available() -> Result<serde_json::Value, String> {
    let exe = synth_exe();
    let mut cmd = Command::new(&exe);
    cmd.args(["--list-available", "--json"]);
    no_window(&mut cmd);
    let out = cmd.output().map_err(spawn_failed)?;
    if !out.status.success() {
        let e = String::from_utf8_lossy(&out.stderr);
        let e = e.trim();
        return Err(if e.is_empty() {
            "Could not reach the voice catalog.".to_string()
        } else {
            e.to_string()
        });
    }
    serde_json::from_str(&String::from_utf8_lossy(&out.stdout))
        .map_err(|e| format!("catalog was not JSON ({e})"))
}

/// Download, verify and unpack one voice. Blocking, and deliberately so: it
/// runs on Tauri's async command pool, not the UI thread, and a 66-232 MB
/// download has nothing useful to say between "started" and "done" that the
/// GUI could show without streaming the child's stderr.
#[tauri::command]
async fn install_voice(id: String) -> Result<(), String> {
    tauri::async_runtime::spawn_blocking(move || {
        let exe = synth_exe();
        let mut cmd = Command::new(&exe);
        cmd.arg("--install-voice").arg(&id);
        no_window(&mut cmd);
        let out = cmd.output().map_err(spawn_failed)?;
        if out.status.success() {
            return Ok(());
        }
        // spfy_synth puts the reason on stderr and nothing else, so it can be
        // shown verbatim rather than translated into a vaguer sentence.
        let e = String::from_utf8_lossy(&out.stderr);
        let e = e.trim();
        Err(if e.is_empty() {
            format!("Installing {id} failed.")
        } else {
            e.to_string()
        })
    })
    .await
    .map_err(|e| format!("install task failed: {e}"))?
}

/// Synthesise and hand the WAV back as a BINARY ipc response, so a 30-second
/// utterance is a memcpy rather than a 240k-element JSON array on the wire.
#[tauri::command]
fn synth(voice: String, text: String) -> Result<tauri::ipc::Response, String> {
    Ok(tauri::ipc::Response::new(synth_wav(&voice, &text)?))
}

/// Write already-synthesised WAV bytes to a path the user picked.
///
/// The bytes make the round trip rather than being kept in Rust because the
/// user may save audio they heard several utterances ago; the browser side
/// owns "which one is on screen", so it owns the buffer.
#[tauri::command]
fn save_wav(path: String, bytes: Vec<u8>) -> Result<(), String> {
    std::fs::write(&path, &bytes).map_err(|e| format!("Could not save {path}: {e}"))
}

/// `spfy_gui --self-check` - prove the engine plumbing works without a window.
///
/// ⚠ THIS IS NOT A DUPLICATE OF THE CLI'S OWN TESTS. Those prove `spfy_synth`
/// works. This proves THIS BINARY can find it, spawn it, and get audio back:
/// the exact functions the IPC commands call, minus only the IPC hop. A GUI
/// whose window opens is not a GUI that speaks, and on a headless CI box the
/// window is all you could otherwise check.
fn self_check() -> i32 {
    let exe = synth_exe();
    println!("engine: {}", exe.display());

    let data = match voices() {
        Ok(v) => v,
        Err(e) => {
            eprintln!("FAIL list_voices: {e}");
            return 1;
        }
    };
    let list = data["voices"].as_array().cloned().unwrap_or_default();
    println!("PASS list_voices        {} voice(s)", list.len());
    if list.is_empty() {
        println!("     search_path: {}", data["search_path"]);
        eprintln!("FAIL nothing to synthesise with");
        return 1;
    }

    let voice = list[0]["name"].as_str().unwrap_or("").to_string();

    // Plain text, then the same sentence under SSML that must CHANGE the
    // audio. Comparing lengths is what separates "SSML was accepted" from
    // "SSML did something" -- and before the SSML pass existed, the tagged
    // arm was LONGER because the engine read the tags out loud.
    let base = "The national weather service has issued a warning.";
    let plain = match synth_wav(&voice, base) {
        Ok(b) => b,
        Err(e) => {
            eprintln!("FAIL synth: {e}");
            return 1;
        }
    };
    if plain.len() < 45 || &plain[0..4] != b"RIFF" {
        eprintln!("FAIL synth returned {} bytes, not a RIFF file", plain.len());
        return 1;
    }
    println!("PASS synth   {:>8} bytes  voice={voice}", plain.len());

    let tagged = match synth_wav(&voice, &format!(
        "<speak><prosody rate=\"x-slow\">{base}</prosody></speak>"))
    {
        Ok(b) => b,
        Err(e) => {
            eprintln!("FAIL synth (ssml): {e}");
            return 1;
        }
    };
    let ratio = tagged.len() as f64 / plain.len() as f64;
    if ratio < 1.5 {
        eprintln!("FAIL ssml    x{ratio:.2} - <prosody rate=\"x-slow\"> did not \
                   lengthen the audio; is this engine build older than the \
                   SSML pass?");
        return 1;
    }
    println!("PASS ssml    {:>8} bytes  x{ratio:.2} vs plain", tagged.len());
    println!("all passed");
    0
}

/// Re-attach to the console that launched us.
///
/// ⚠ Needed because of the `windows_subsystem = "windows"` attribute at the
/// top of this file. That attribute is right -- without it every launch of the
/// GUI pops a console window -- but it also means a release build has NO
/// stdout, so `--self-check` would run correctly, set the right exit code, and
/// print absolutely nothing. Called before the first println!, because Rust
/// resolves the stdout handle lazily on first use and caches it.
#[cfg(windows)]
fn attach_console() {
    const ATTACH_PARENT_PROCESS: u32 = 0xFFFF_FFFF;
    #[link(name = "kernel32")]
    extern "system" {
        fn AttachConsole(dwProcessId: u32) -> i32;
    }
    unsafe {
        AttachConsole(ATTACH_PARENT_PROCESS);
    }
}

#[cfg(not(windows))]
fn attach_console() {}

fn main() {
    if std::env::args().any(|a| a == "--self-check") {
        attach_console();
        std::process::exit(self_check());
    }

    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .invoke_handler(tauri::generate_handler![
            list_voices, list_available, install_voice, synth, save_wav])
        .run(tauri::generate_context!())
        .expect("error while running spfy GUI");
}
