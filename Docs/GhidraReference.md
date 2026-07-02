# Ghidra Reference — Verified Facts for FFXII Project

This document is **ported from DQ7R-Screen-Reader's `GhidraReference.md`** (every rule
there exists because a previous session destroyed analysis data or wasted hours). Where
the rule is FFXII-specific, it's marked `(FFXII)`; otherwise it's verbatim from DQ7R.

If it's not in this document, **DO NOT make claims about Ghidra behavior.**

---

## 1. Saving Behavior

### Headless Mode (`analyzeHeadless`)
- **Saves exactly ONCE — at the very end**, after all analysis + scripts complete.
- There is NO incremental saving, NO periodic flushing, NO checkpointing during analysis.
- If the process is killed mid-analysis: **all new work since startup is lost**.
- `-process` mode preserves the PREVIOUS save — the old `.gbf` is not corrupted.
- `-import` mode: if killed before completion, the program is never written to the project.

### GUI Mode
- Manual save: File → Save (Ctrl+S).
- Recovery snapshots may not be created reliably for large projects.
- **Use headless for all analysis.** GUI is only for browsing.

### NET RESULT
**Neither GUI nor headless provides reliable mid-analysis saving for large
executables.** For FFXII (~30-50 MB) this is less of a concern than for DQ7R
(~200 MB), but the rule still holds: backup `.rep` directory before each long
analysis run.

---

## 2. Database Structure (`.rep` directory)

```
FFXII.gpr              -- Empty marker file
FFXII.lock             -- Lock file (process holding the project open)
FFXII.lock~            -- Lock backup
FFXII.rep/             -- All project data
  projectState
  idata/
    ~index.dat
    00/
      00000000.prp     -- Program properties
      ~00000000.db/
        db.1.gbf       -- Database version 1 (previous save)
        db.2.gbf       -- Database version 2 (latest save)
        tmp*.ps        -- Temp property store (active session, NOT saved)
  user/
  versioned/
```

**Key facts:**
- `.gbf` files are written ATOMICALLY as new files (not updated in-place).
- `tmp*.ps` files are temp working copies — NOT saved analysis data.

---

## 3. Single-Shot Pipeline (FFXII)

`FFXII_TZA.exe` is small enough that DQ7R's "phased analysis" + `batch_decompile`
approach is overkill. We use a single-shot script `import_analyze_decompile.java`
that does the whole pipeline. **Do not port `disable_expensive_analyzers.java` or
`batch_decompile.java` — confirmed they don't pay off at this binary size.**

---

## 4. JVM Memory Configuration

Default headless: 2GB — too small for binary analysis. Set
`GHIDRA_HEADLESS_MAXMEM=8G` before running. For FFXII (~30-50 MB), 8G is
comfortable; 4G is the floor.

---

## 5. Key GhidraScript API Methods for Saving

```java
saveProgram(currentProgram);

// Lower-level alternative:
end(true);
currentProgram.getDomainFile().save(monitor);
start();

// Prevent saving (search/decompile scripts MUST do this):
currentProgram.setTemporary(true);
```

**`saveProgram()` can be called from pre/post scripts but NOT during auto-analysis.**

---

## 6. `-import` vs `-process`

| Command | Effect |
|---------|--------|
| `-import binary.exe` | Creates new project entry + analyzes. Safe if entry doesn't exist. |
| `-import binary.exe -overwrite` | **DESTROYS existing entry** — never use after initial import. |
| `-process binary.exe` | Opens existing entry, runs analysis/scripts. Safe. |
| `-process binary.exe -noanalysis` | Opens existing, runs scripts only. Safest. |

**RULE: After initial import, NEVER use `-import` again. Always `-process`.**

DQ7R session 95: generic fallback used `-import -overwrite` and destroyed 12 hours of
analysis. Do not repeat.

---

## 7. Ghidra 12 API Gotchas (FFXII project uses Ghidra 12)

- `getSourceFile()` returns `ResourceFile`, not `File`. Wrap with
  `new File(getSourceFile().getAbsolutePath())`.
- `Listing.getFunctionBefore()` removed. Use `getFunctions(addr, false)`.
- `ReferenceManager.getReferencesTo()` returns `ReferenceIterator`, NOT `Reference[]`.
  Iterate with `while (refs.hasNext())`.
- All search/decompile scripts MUST call `currentProgram.setTemporary(true)` at the
  start to prevent accidental saves.
- `toAddr(long)` for address conversion from vtable reads — NEVER `imageBase.getNewAddress()`.
- All loops include `monitor.isCancelled()` checks.

### Standard import block (Ghidra 12 — copy verbatim into new scripts)

```java
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import ghidra.util.task.TaskMonitor;
import generic.jar.ResourceFile;
import java.io.*;
import java.util.*;
```

Missing imports cause `ClassNotFoundException` and the script is silently skipped
(zero output). Always match imports to APIs used.

---

## 8. Output Archiving Rule (FFXII)

Every Ghidra script's output goes to `..\FFXII-Decompile\output\`. **Never overwrite**
— use `cp -n` (or rename with timestamp) before re-running a script.

`new FileWriter(outFile)` OVERWRITES — always copy output before re-running.

---

## 9. Backup Strategy

Before each analysis phase:
```batch
xcopy /E /I /Y "projects\FFXII.rep" "projects\backups\FFXII_pre_<phase>.rep"
```

This is the ONLY reliable protection against data loss.

---

## 10. What Ghidra CANNOT Do

- Cannot save incrementally during analysis.
- Cannot resume a killed analysis session — re-running `-process` restarts from the
  last successful save.
- Cannot save from an external process (no IPC trigger).
- Cannot reliably create recovery snapshots for large projects.

---

## Sources

Ported from `D:\Games\Dev\unreal\dq7-r\DQ7R-Screen-Reader\Docs\GhidraReference.md`
(commit unknown, snapshot date 2026-05-04). FFXII-specific rules added inline.
