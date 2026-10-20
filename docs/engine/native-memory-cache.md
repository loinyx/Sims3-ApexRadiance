# Native memory cache

## Scope

Steam 1.67.2.024037, executable timestamp `0x52DEC247`, image base `0x00400000`. Other executable layouts are not
validated for the public memory test's partial cache step.

## Evidence

ResourceSystem update `0x00737560` calls `0x00732E90` twice, with zero arguments, on pointers at `this+0x1E0` and
`this+0x1E4`. Shrink-both `0x00733E70` calls the same function with `(cache+0x8C, null, true)`. The callee returns with
`ret 0xC`, matching three stack arguments and a this pointer in ECX.

Trim subtracts its first argument from the field at `cache+0x8C`. Its loop compares that target against the sum of
`cache+0x94` and the running idle-accounting value initially read from `cache+0x98`. It decrements the latter by
selected entry sizes and writes the result back. The third argument bypasses a minimum-idle check at `cache+0x90`.
Deferred entries are destroyed after the cache lock is released. This establishes the partial-target calculation;
field names beyond idle accounting and the impact on OS memory remain unverified at runtime.

## Application

The experiment validates the native entry and shrink-both argument bytes before enabling partial steps. It computes
`budget - (other + idle) + requestedReduction` only when the sum fits the budget. A maximum requested reduction of
16 MiB and a retained idle target of 64 MiB avoid an unconditional shrink-both operation during gameplay. The caller
runs after the original ResourceSystem update. Concurrent changes and whole-entry granularity prevent a strict byte
or time bound; cooldown and slow/no-benefit backoff limit repeated work.

## Open checks

Measure actual process commit and address-space changes, native-call stalls, reload traffic and repeated world travel.
Do not infer which textures, lots or script objects own a cache entry from these accounting fields alone.
