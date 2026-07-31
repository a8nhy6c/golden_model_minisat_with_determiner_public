# MiniSAT Grounded Golden Model for the HW-BCP CAM+SRAM Submodule

An operation level golden model whose read / write / search semantics are executed by the real
MiniSAT solver (bundled in `minisat/`), not by hand written model code. It is timing free at the
modeling level: you describe operations as a small C++ program against a builder API, and the same
program emits both a human readable results log and a timed, self checking `.vec` for Cadence
Virtuoso. A separate Python script then turns that `.vec` back into a human readable markdown report
so the expected outputs can be checked by eye.

```
  ops program (examples/*.cpp, hand written)
  write / search / read calls on GoldenModelRun
            |  compiled against the real minisat Solver.cc
            v
  +- results .txt   human readable log: expected Q_Val, Q_Pol, VID_OUT, Val_OUT, Pol_OUT
  |                 and the per clause CONF / UP / DONE / Lit_Pos after every op
  |                 (never parsed by anything)
  +- checked .vec   timed stimulus + io o expected columns (VecEmitter owns ALL timing)
            |         emitCheckedVec               -> bare Submodule_4x16, memory columns
            |         emitCheckedVecWithDeterminer -> BCP_Top_4x16_4det, + determiner columns
            |                                   |
            v                                   v
  Cadence tran sim -> pass or fail    vec_to_markdown.py -> report.md (tables per op)
```

## Status

This copy models the CAM + SRAM submodule together with the per row Determiner that evaluates a
clause from the stored literal values. It computes `CONF`, `UP`, `DONE` and `Lit_Pos` for every
clause after every operation (section 7), and can emit them as extra checked columns in the `.vec`.

Not modelled: the H tree logic that would combine per clause verdicts into a chip level answer,
and the step from `Lit_Pos` back to a variable ID. Neither exists in the hardware wrapper yet.

## 0. Requirements

- Your computer can run C++
- Python 3 (also shipped with the command line tools) for the `.vec` to markdown report.

No other dependencies. The MiniSAT solver is bundled in `minisat/`, so there is nothing to download.

## 1. Build and run

```bash
make                                  # compiles the bundled minisat + both example ops programs
./examples/expanded_all_ops           # writes examples/out_results.txt + examples/out_checked.vec
./examples/determiner_ops             # writes examples/out_determiner_results.txt
                                      #    + examples/out_determiner.vec
make examples/template_ops            # build the skeleton you edit (section 2)
make examples/expanded_all_ops        # (re)build one ops program
make clean                            # remove build artifacts and generated outputs
```

`make` builds only the two worked examples. `examples/template_ops.cpp` is built on demand, so a
half finished edit in it never blocks the rest of the project from compiling.

The build uses `-std=gnu++98` because minisat is C++98 era code; clang in c++11 mode rejects its
`"%4"PRIi64` style literals. The Makefile compiles `minisat/minisat/core/Solver.cc` and
`minisat/minisat/utils/System.cc` from the bundled copy (override the location with `MINISAT=...`).

## 2. Writing an ops program to generate a new .vec

**Start with `examples/template_ops.cpp`.** It is a skeleton with an empty body and every
operation documented, with examples, in one comment block at the bottom of the file. Three steps:

**Step 1. Type your operations** where the marker line is:

```cpp
    goldenmodel::GoldenModelRun run(16, 4);

    // type your operations here
    run.write(0, 6, '1', 1);
    run.search(6, '0');
    run.read(0);

    if (run.emitResults(results_path) != 0) return 1;
```

**Step 2. Build and run**, from the top of this project, not from inside `examples/`:

```bash
make examples/template_ops
./examples/template_ops
```

**Step 3. Read the output.** `examples/out_template_results.txt` is the log to check by eye,
`examples/out_template.vec` is what Cadence consumes. To put them somewhere else, pass two
arguments: `./examples/template_ops my_results.txt my_stimulus.vec`.

That is the whole loop. Edit, `make examples/template_ops`, run. Nothing else needs regenerating.

### The three operations

```cpp
run.write(0, 6, '1', 1);      // row 0: vid 6, value '1', polarity 1
run.search(6, '0');           // every row holding vid 6 takes value '0'
run.read(0);                  // strobe row 0 onto VID_OUT / Val_OUT / Pol_OUT
```

| | what it does |
|---|---|
| `write(row, vid, val, pol)` | addressed write of one row. Optional fifth argument `wecam`, default 1; pass 0 to write only the SRAM value and polarity and leave the row's stored vid alone |
| `search(vid, val)` | CAM search plus the matchline activated SRAM write. Non matching rows are untouched. `val` of `'x'` unassigns the variable, which is how a backtrack is modelled |
| `read(row)` | addressed read. Changes no stored state, so it produces no table in the markdown report |

`val` is the character `'0'`, `'1'` or `'x'`, where `'x'` means the variable is unassigned. `pol`
is 0 for a plain literal `x_i` and 1 for a negated literal `NOT x_i`. Any other `val` character, an
out of range row, or an out of range vid throws with the operation number.

`GoldenModelRun(16, 4)` sets 16 rows and 4 VID bits. The row count must be a multiple of 4, since
each Determiner covers 4 consecutive rows as one clause, and 4 VID bits allow vid 0 to 15. Both
must match the submodule netlist you intend to simulate against.

Because operations are C++ rather than parsed text, a malformed one (wrong argument count or type)
is a compile error rather than a runtime surprise.

### Choosing which .vec to emit

The template calls `emitCheckedVecWithDeterminer`, which adds five checked columns per Determiner
and targets the wrapper containing them. For the memory only stimulus aimed at the bare
`Submodule_4x16`, swap that one line for `emitCheckedVec`. The determiner verdict is computed
either way and always appears in the results log; the choice only changes which columns land in
the `.vec`.

### Worked examples to copy from

- `examples/expanded_all_ops.cpp` is a three operation minimum: write, search, read.
- `examples/determiner_ops.cpp` drives eight clause scenarios and documents the expected
  `CONF` / `UP` / `DONE` / `Lit_Pos` for each, taken from the Determiner's own reference vectors.
  Useful as a correctness reference when you are unsure what a verdict should be.

Build any of them the same way, with `make examples/<name>`.

### 2.1 The 2 bit Val encoding

The three literal values are stored in two SRAM cells per row, driven as `Val_IN_1 Val_IN_0` and
read back on the `Q_Val` nodes:

| `val` character | `{Val_1, Val_0}` | meaning |
|---|---|---|
| `'0'` | `00` | literal value false |
| `'x'` | `01` | variable unassigned |
| `'1'` | `11` | literal value true |
| n/a | `10` | illegal, never written |

The codes are ordered false < x < true along the value axis, which leaves `10` as the single
unused pattern. That is what lets the downstream Determiner treat `10` as a detectable error
rather than silently misreading it, so a model bug that produces an impossible code shows up as a
vec failure instead of a plausible looking wrong answer.

This is the encoding the Determiner decodes in `DET_StageOne.v`, whose truth table over
`{Val1, Val0, Pol}` reads `11x` as `ONE` / `INV_ZERO`, `00x` as `ZERO` / `INV_ONE`, `01x` as
`UNKNOWN` / `INV_UNKNOWN`, and `10x` as `ERROR0` / `ERROR1`. It is also consistent with the
load then search ordering in the hand written `genVecFile.py` stimulus, which initializes every
row to `01` while loading the SAT problem (all variables unassigned) and only then searches in
`11` and `00`.

The three legal codes are named `VALBITS_FALSE`, `VALBITS_UNKNOWN`, and `VALBITS_TRUE`, defined
once at the top of `golden_model_core.cpp` and used by the four conversion helpers directly below
them. Nothing else maps a literal value onto a bit pattern, so changing the encoding means editing
those three constants and nothing more. (Read operations separately drive both `Val_IN` pins low,
copying the reference vec; that is a stimulus convention rather than a value, so it does not go
through the constants.)

## 3. Results log format (emitResults)

```
config n=16 k=4

op 1 write row=0 vid=0 val=1 pol=0
q_val 11------------------------------
q_pol 0---------------

op 21 search vid=6 val=0
matched rows=2,7,10
q_val 01010001010101000101000101010101
q_pol 0010010001100000

op 22 read row=2
read vid=0110 val=00 pol=1
q_val ...
q_pol ...
conf_det 0---
up_det 1---
done_det 0---
lid_det 01------
```

- `q_val` has 2n characters; character `2r` is the MSB storage node `Q_Val[2r]` of row r and `2r+1`
  the LSB, matching the generated netlist wiring (`Q[2r]` maps to `BL[1]`). `q_pol` has n
  characters. `-` means the row was never addressed written, so its power up state is indeterminate
  and Cadence must not compare it.
- `conf_det` / `up_det` / `done_det` have one character per determiner and `lid_det` has two, MSB
  first. `-` means that clause contains at least one never written row, so no verdict is defined.
  These four lines appear whichever vec flavour you emit, since the verdict is computed either way.
- `matched rows=` appears after every search (informational; `CAM_ML` is not a port, so match
  correctness is verified indirectly through the Val write it causes).
- `read vid= val= pol=` appears only after read operations and maps onto `VID_OUT_{k-1}..0` (MSB
  first), `Val_OUT_1 Val_OUT_0`, `Pol_OUT` on the SenseEN strobe line. Reading a never written row
  gives `-` fields.

This file is a log for humans; nothing parses it back. The checked `.vec` is produced from the same
in memory snapshots.

## 4. Turning the .vec into a human readable report (vec_to_markdown.py)

The final workflow stage parses a checked `.vec` back into a markdown document so a human can verify
the expected outputs the vec asserts:

```bash
python3 vec_to_markdown.py examples/out_checked.vec report.md   # or omit report.md for stdout
```

The `.vec` is the ONLY input: the script does not read minisat, `golden_model_core.cpp`, or the
results log. Everything it prints is reconstructed from the vec's own `vname` header and data lines.
The report is a sequence of tables, one appended per **write or search** operation (reads change no
storage, so they get no table). Each table lists all n rows with columns **Row, VID, Q_Val, Q_Pol**
for the settled state after that operation:

- op boundaries and the op label come from the `; write ... / ; search ... / ; read ...` comment
  lines already present in the vec;
- per row VID is recovered from the `ADDR_IN_*` and `VID_IN_*` input columns of write operations
  whose `WE_CAM` is asserted (a `wecam=0` write leaves the row's VID unchanged);
- the settled `Q_Val` / `Q_Pol` for an operation are read from the first data line of the next
  operation's block, which by the emitter's strobe policy (section 8) carries the previous
  operation's settled snapshot; this uniformly captures both a write's own commit and a search's
  deferred commit. `-` cells are passed through verbatim (never written / do not compare).

If the vec carries determiner columns, a second table per operation is appended with **Determiner,
Rows, CONF, UP, DONE, Lit_Pos**. Those values come from the operation's own **last** data line, its
settle row (section 7), not from the next operation's block. On a vec without determiner columns
the script behaves exactly as before.

## 5. Bundled MiniSAT

`minisat/` is a redistributed copy of upstream MiniSAT 2.2
(https://github.com/niklasso/minisat), under its own MIT license (`minisat/LICENSE`). It carries two
small build fixes for clang, with no behavior change:

1. `minisat/minisat/core/SolverTypes.h`: the default argument of `mkLit` is moved from the friend
   declaration to the inline definition. Clang rejects a default argument on a friend declaration in
   every language mode; this is the well known minisat on clang fix.
2. `minisat/minisat/utils/System.cc`: the FreeBSD / Apple / fallback definitions of `memUsedPeak`
   gained the `bool strictlyPeak` parameter that `System.h` declares. Upstream only the Linux branch
   matched the header.

## 6. Files

```
golden_model_minisat_with_determiner_public/
|-- golden_model_core.cpp             HwBcpSolver + OperationModel + VecEmitter + GoldenModelRun
|-- vec_to_markdown.py                .vec -> markdown report (the only input is the .vec)
|-- Makefile                          all / clean; pattern rule builds examples/<name>.cpp
|-- LICENSE                           MIT (project) + note on bundled MiniSAT
|-- examples/template_ops.cpp         START HERE: empty skeleton, all three ops documented
|-- examples/expanded_all_ops.cpp     minimal worked example: write, search, read
|-- examples/determiner_ops.cpp       determiner scenarios with known good expected verdicts
`-- minisat/                          bundled, lightly patched MiniSAT 2.2 (its own LICENSE)
```

## 7. The Determiner model

`DET_Hier` evaluates one clause of four literals and emits `CONF` (all literals false), `UP`
(exactly one unassigned, the rest false), `DONE` (at least one true), and a 2 bit `Lit_Pos` naming
which literal to propagate. Determiner `d` covers rows `4d` to `4d+3` as literals A, B, C, D, the
grouping the hardware wrapper wires up.

Three pieces make up the model, all in `golden_model_core.cpp`:

- `evaluateLiteral` mirrors `DET_StageOne`. It decodes the row's own stored `{Val1, Val0}` nodes,
  not the solver's global state, because that is what the SRAM presents to the logic, then folds
  polarity with MiniSAT's own `lbool::operator^`, the identical operation `Solver::value(Lit)`
  performs. So MiniSAT owns the three valued logic and the polarity fold.
- `evaluatePair` and `evaluateDeterminer` transcribe the `DET_StageTwo` case table and the five
  `DET_StageThree` assignments, error propagation terms included. An illegal `10` code therefore
  reaches the output as the `CONF=UP=DONE=1` signature exactly as the gates would produce it.
- `verifyDeterminersAgainstSolver` rebuilds the clause as real `Lit`s and compares the real
  `Solver::satisfied` against the modelled `DONE`, printing a `warning:` line on mismatch. It never
  changes an emitted value.

`Lit_Pos` cannot come from MiniSAT: `propagate()` reorders clause literals in place, so an index
into a MiniSAT clause is not stable, while hardware rows never move. Position therefore comes from
the fixed row grouping and the mirrored tree. That is also why the clause is built through the
allocator directly rather than through `addClause_`, which sorts.

**Never written rows.** If any row of a clause has never been written, all five of that
determiner's columns are `-`. Power up state is indeterminate, so no verdict is defined.

**When the MiniSAT cross check runs.** An addressed write updates only the row, never the solver,
which is what the hardware wordline does. Row state and solver state therefore diverge by design
after a write, and only a search resyncs them. The check is skipped while they disagree, so in
practice it fires after searches. `examples/determiner_ops.cpp` ends with a search pass for exactly
this reason.

**Tap order.** `DETERMINER_READS_EVEN_NODE_AS_VAL1` selects which `Q_Val` node feeds `Val1`. The
default `true` matches the array wiring, where `Q[2r]` carries `BL[1]` and therefore `Val_IN[1]`.
Setting it to `false` reproduces the opposite tap order, including the illegal code a stored `x`
then produces. It exists so the model can predict either wiring from a one line edit.

## 8. Vec timing and the settle row

`VecEmitter` owns all timing; the model itself is timing free. Each operation gets a 10 ns slot,
and the offsets within it are transcribed from the hand written reference stimulus rather than
computed:

| Operation | Control rows | Commit | Settle row |
|---|---|---|---|
| write | +0 precharge, +5 write driver, +6 wordline on, +7 off | +6 | +8 |
| search | +0 SL precharge low, +2 ML precharge high, +5 drivers, +7 wordline enable | +7 | +9 |
| read | +0 bitline precharge, +5 ReadEN, +6 wordline, +10 SenseEN strobe | none | none |

Any operation following a read starts at +2 instead of +0 so it clears the previous read's 2 ns
SenseEN pulse.

**Strobe policy.** `Q_Val` / `Q_Pol` hold the previous settled snapshot before the commit row, are
all `-` on the commit row, and hold the new snapshot from the first row after it. For a search the
commit is the last control row, so its settled value first appears in the next operation's block.
Reads change no storage, so their rows keep the current snapshot, and `VID_OUT` / `Val_OUT` /
`Pol_OUT` are asserted only on the SenseEN row.

**Why the determiner gets its own assertion point.** A write commits at +6 and asserts `Q_Val` at
+7, a 1 ns window, and the determiner's logic delay stacks on top of the SRAM settling that 1 ns
was tuned for. Every stimulus file in the Determiner's own `vectorFile/` directory spaces its input
changes 2 ns apart, and one of them exists purely to isolate edges for delay measurement, so 2 ns
is the only empirical statement available about how long this block needs.

So write and search each gain one trailing **settle row** at `commit + DETERMINER_SETTLE_OFFSET`,
carrying a verbatim copy of the preceding row's control tokens. No control signal changes value on
it; it exists only to give the determiner columns somewhere to be asserted. Determiner columns hold
the previous verdict before the commit, are `-` from the commit row up to the settle row, and carry
the new verdict on the settle row. Reads add no settle row. Both offsets stay inside the 10 ns
slot, so nothing outside these columns moves, and `emitCheckedVec` never takes this branch, which
is why the memory only vec is byte for byte what it was before.

The header's rail relative thresholds `voh 0.77` / `vol 0.33` / `vth 0.55` (0.7, 0.3, 0.5 of
VDD = 1.1) now also apply to the determiner columns. They were chosen for SRAM storage nodes;
confirm they suit standard cell outputs before trusting a pass.
