# Frida Scripts — Canonical Patterns for FFXII

Ported from `DQ7R-Screen-Reader\Docs\FridaScripts.md` (or its equivalent location in
`memory/`). Every helper here exists because a custom alternative wasted a session.

**USE the canonical patterns. Do not invent alternatives.** If a custom helper differs
from the documented version, replace it with the documented version before declaring
the script ready.

---

## 1. Module base address — canonical pattern

```javascript
// CORRECT — works in injected scripts:
const base = Process.enumerateModules()[0].base;

// WRONG — does NOT exist in Frida's QuickJS injected runtime:
// const base = Module.findBaseAddress("FFXII_TZA.exe");  // throws
```

Every script in this project uses `Process.enumerateModules()[0].base`. The first
enumerated module is always the main executable.

---

## 2. RVA + read helpers

```javascript
function rva(offset) { return Process.enumerateModules()[0].base.add(offset); }

function safeReadPtr(addr) {
    try { return addr.readPointer(); }
    catch (e) { return ptr(0); }
}

function safeReadU32(addr) {
    try { return addr.readU32(); }
    catch (e) { return 0; }
}

function safeReadFloat(addr) {
    try { return addr.readFloat(); }
    catch (e) { return 0.0; }
}
```

---

## 3. NativePointer — what does NOT exist

- `NativePointer.toNumber()` does not exist. Use `.toInt32()` or `.toUInt32()`.
- `Module.getExportByName()` does not exist as a static method in injected scripts.
- `setInterval()` / `setTimeout()` do not exist. For periodic work use a Python host.
- `File.read()` does not exist. The `File` class is write-only. Hardcode reference data.
- `Script.bindExitHandler()` is a Node.js host-side API, not available in injected
  scripts.

---

## 4. Hooking a function (Interceptor.attach)

```javascript
const targetRva = 0x12345; // discovered via Ghidra
const target = rva(targetRva);

const handle = Interceptor.attach(target, {
    onEnter(args) {
        // args[0..3] are RCX/RDX/R8/R9 on x64 Windows
        const arg0 = args[0];
        try {
            send({ type: 'hit', rva: targetRva.toString(16), arg0: arg0.toString() });
        } catch (e) {}
    },
    onLeave(retval) {
        // retval can be modified
    }
});

// Detach when done:
// handle.detach();
```

---

## 5. Watching a memory location for writes

```javascript
const addr = rva(0x12345);

MemoryAccessMonitor.enable({ base: addr, size: 8 }, {
    onAccess(details) {
        send({
            type: 'mem-access',
            operation: details.operation, // 'read' / 'write' / 'execute'
            from: details.from,
            address: details.address,
        });
    }
});
```

Use this to find write-sites for known fields (HP, MP, time scale, etc.) without
reading the whole binary.

---

## 6. Reading wide strings

```javascript
function readWide(addr, maxLen = 256) {
    try {
        let out = '';
        for (let i = 0; i < maxLen; i++) {
            const c = addr.add(i * 2).readU16();
            if (c === 0) break;
            out += String.fromCharCode(c);
        }
        return out;
    } catch (e) { return ''; }
}
```

For FFXII text — encoding is TBD. May not be UTF-16; could be a custom multi-byte
scheme. Phase 1 task: find the byte→glyph decoder function.

---

## 7. send() / recv() to a Python host

For periodic checks, keyboard input, or anything time-based, use a Python host:

```python
# host.py
import frida, sys, time

def on_message(msg, data):
    if msg['type'] == 'send':
        print(msg['payload'])

session = frida.attach('FFXII_TZA.exe')
with open('probe.js', 'r') as f:
    script = session.create_script(f.read())
script.on('message', on_message)
script.load()

# Drive the script from here — keyboard polling, timers, etc.
while True:
    time.sleep(0.5)
    # script.post(...) to send commands into the injected script
```

`@auto` tag on a probe script means run-once / no Python host required.

---

## 8. Output budget for screen-reader compatibility

The user is blind and uses NVDA. Dumping hundreds of lines via `console.log` /
`send()` crashes the screen reader.

**Rules:**
- Per-call hooks must use file logging for verbose data; console gets a one-line
  summary.
- For ProcessEvent-style high-frequency hooks, console output must be
  O(unique_events), not O(N).
- `logFile(msg)` helper writes to a file; `console.log()` is the budget channel.

```javascript
const LOG_PATH = "D:/Games/Dev/Custom/FFXII/FFXII-Decompile/notes/probe_<topic>.log";
let logFileHandle = null;
function logFile(msg) {
    try {
        if (!logFileHandle) logFileHandle = new File(LOG_PATH, "w");
        logFileHandle.write(msg + "\n");
        logFileHandle.flush();
    } catch (e) {}
}
```

**NEVER write logs to the game's CWD.** Use the absolute path under `FFXII-Decompile\notes\`.

---

## 9. Probe lifecycle

Every probe script starts with:
```javascript
console.log('[probe_<topic>] base=' + Process.enumerateModules()[0].base);
```

…and ends with:
```javascript
console.log('[probe_<topic>] hooks installed; waiting for events…');
```

These two lines are the user's signal that the probe is alive.

---

## 10. Reference

DQ7R's source for these patterns:
`D:\Games\Dev\unreal\dq7-r\DQ7R-Screen-Reader\Docs\FridaScripts.md` (or
`memory/FridaScripts.md`).
