FFXII CORE REGRESSION CHECKS

From the mod source directory in PowerShell:
  cmd.exe /d /c tests\run_tests.bat

Uses the workspace dev_env.bat tool paths and an x64 MSVC/Ninja build.
Exercises the actual decoder, logger and speech code. Runtime hook services and
Tolk are private test stubs; these checks do not access the game or screen reader.
The fake Tolk DLLs stay in build/core-tests-ninja/speech-good and speech-missing.
Never copy these test DLLs into a game or a release bundle.

Success: three CTest executables pass. Details are written to:
  build/core-tests-configure.log
  build/core-tests-build.log
  build/core-tests-results.log

Core cases cover token payload zeros/truncation, page boundaries, protected
memory, nested fields/decodes, concurrent atlas changes and in-flight overrides,
and bounded UTF-8 conversion. Speech cases cover accepted/rejected output,
queue/interrupt/mute semantics, repeated init and concurrent calls/shutdown,
and cleanup after missing exports. Game behavior still needs play testing.
