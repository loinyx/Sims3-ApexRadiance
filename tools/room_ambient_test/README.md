# Room ambient checks

From the repository root, with the installed MSVC x86 Build Tools and existing project vcpkg dependencies:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/room_ambient_test/run.ps1
```

The runner uses `vswhere` to locate the existing compiler, builds Release-style x86 executables in
`%TEMP%/ApexRoomAmbientTests`, then runs both suites. `-OutDir` and `-VsDevCmd` override those paths. It does not install
dependencies, attach to TS3W, change game files or write test data from the executables.

- `room_ambient_test.cpp`: production scheduling and ambient policy, and the kept wall rows beyond an edge (`FollowGhosts`) (153 checks).
- `unlit_rooms_recovery_test.cpp`: includes `features/unlit_rooms.cpp` directly, preserving its real update and recovery
  logic. Windows time, the native room queue, group updater, address lookup and patch-write services are fixtures. The
  suite uses x86 buffers matching the accessed room fields and exercises 1,349 checks, including continuous slider
  sweeps and the day/night/dark-room furniture combinations.

The original updater fails the rejected-queue restoration, small first-family brightness steps and unset high-uptime
deadline cases. The repaired updater passes all 1,438 checks across both suites. A queue acknowledgement test also ensures that a fresh
base captured during queuing survives the older request's acknowledgement.

The suite exercises `AddBase` with controlled native top-up results and verifies group-service delegation. It does not
execute TS3's original top-up function, lot enumeration, group solver, installed binary hooks or D3D9 draws. These limits
matter when interpreting the count: it includes property checks at many slider values, rather than 1,421 distinct
gameplay scenarios.

## Reference brightness comparison

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/room_ambient_test/run.ps1 -CompareTag v2.5.5
```

This also compiles the updater read from the named Git tag and runs the 63 brightness-reference checks against it.
The fixtures include the actual 2026-10-03 session's inherited RGB (.01, .01, .01), its fourth components, and the
standard blue second family (.15, .15, .30). Both the current updater and v2.5.5 pass, returning the same ambient:
100% of the inherited grey is still grey with luma .01, whereas the standard blue has luma .16083. A blue-retention
control cannot recover chroma that a different setting has removed from the base. First-family grey remains .01;
it must not be replaced with the second family's blue. The source from Git and the compiler wrapper are written only
to the specified test output directory.

## Runtime validation

Use a test build after installing it through the normal explicitly authorised game workflow. Record the binary hash
and use F6 if a response differs; the recorder includes actual control values, room ambient/solver state and furniture
paths. Check these scenarios in the same scene:

1. Enter a world at night. Sweep Brightness 100% → 0% → 100%, then Blue tint 100% → 0% → 100%. Check walls, floors and
   furniture in rooms without lamps and in rooms with lamps. Actual lamps should keep their colour and contribution.
2. Repeat while dragging quickly, switching lamps, changing floors and moving between lots. After the final change
   settles, rooms previously being solved should catch up without an extra manual refresh.
3. Enable and disable the adjustment repeatedly. Both lit and unlit rooms should return to the game's ambient when
   disabled. Re-enter the world, and load a second save, without changing the saved settings.
4. Check dawn/daytime and a dark enclosed room during the day. Furniture in daylight should retain the normal day
   factor, while the native NoLight dark-room override should follow the controls. Compare the same materials and camera.

A passing offline suite establishes the tested logic and fixtures. Visual correctness across every game version,
material, render backend and third-party mod needs runtime evidence.

The production harness checks S3SS override preparation (including invalid/inline/CRLF TOML), preservation of unrelated settings, that enabling Rooms at Night never invokes the correction, the explicit correction path and already-applied RGB baseline, both room families, furniture tint, and 10/35/80% brightness with zero blue. Actual config file writes remain outside this read-only harness.
