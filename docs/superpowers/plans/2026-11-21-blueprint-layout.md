# Blueprint layout correction

Goal: repair Blueprint event/function layout without changing logic or user assets.
Design: use synchronous off-screen Slate node layout to measure normal-detail body bounds and pin centres. No UI tick/async callback, no dependency on an open asset editor; retain estimates when measurement is unavailable. Keep unselected nodes fixed. Reserve under-row data lanes and keep terminal completion paths right of their upstream execution region. Shared source identity is preserved. The user subsequently explicitly authorized local self-member Get copies for distant reads; nearby array consumers share one, Swap and Return get separate local reads. Calculations and execution outputs are never duplicated.

## Tasks
- [x] Capture current graphs and reproduce geometry errors with native regression tests (Length/Get/Swap, function entry/result).
- [x] Implement measured geometry with validated fallback; test shared nodes and loop completion geometry in solver.
- [x] Compile Mac UE 5.8.4 Universal, run native automation tests, review diff.
- [x] Duplicate BubbleSortTest under a unique test path; snapshot its nodes, pin defaults and links; arrange all three graphs, verify no semantic changes and inspect real UI.
- [ ] Keep original user asset and dirty level unchanged; install only after successful checks, with backup and coordinated editor restart.

Constraints: preserve existing MCP and project settings, no Save All, no push, one editor writer at a time. Never restart an editor with unsaved user work without consent. Successful compilation is separate from runtime and visual validation.

Validation ledger: old native geometry regression failed at Length 112x34 versus native 219x76 and Swap height 126 versus 143; terminal completion also reproduced. An initial invalid transient graph fixture crashed its isolated process; corrected by creating a full transient Blueprint context. Four automation suites now pass on arm64, including copied Blueprint event + two functions, complete pin/value provenance, FKey default isolation, convergent execution routes and four actual integer-sort inputs. Original .uasset hashes unchanged. Final UE 5.8.4 Universal UBT build succeeded (148 actions), including arm64 and x86_64 for all three modules; package verification follows. User subsequently explicitly authorized direct plugin replacement. Back up and atomically replace the on-disk plugin, retain the running editor and unsaved work; the new native code takes effect after a user-approved restart.
