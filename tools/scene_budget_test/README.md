# Scene budget checks

The runner extracts `UpdateTail`, `MustNotWait` and `BudgetedDrain` from production and compiles x86 developer and
public fixtures. Synthetic nodes use the native link and owner offsets; callbacks use the real update vtable slot.

Run `tools/scene_budget_test/run.ps1 [-OutDir <folder>] [-ReferenceSource <scene_budget.cpp>]`.

The checks cover every combination of mandatory updates in sixteen deferred nodes, normal count and expired time
budgets, empty lists, allocation fallback, removal from either local list during a callback, new nodes queued during
an update, self-requeue and developer validation failure. The fixture checks update order, remaining order, link
consistency and counters. Locks, allocation failures and node lifetime checks are controlled fixture dependencies;
this is not a concurrent native-game or performance measurement.
