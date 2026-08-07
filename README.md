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
            |         emitCheckedVecFullChip       -> decoder + array + determiners + tree,
            |                                        + D_out and tree columns
            |                                   |
            v                                   v
  Cadence tran sim -> pass or fail    vec_to_markdown.py -> report.md (tables per op)
```

## Status

This copy models the whole chip: the `Decoder_4to16`, the CAM + SRAM submodule, the per row
Determiner that evaluates a clause from the stored literal values, and the two level Combining
Tree that reduces the four clause verdicts to one chip level answer. It computes `CONF`, `UP`,
`DONE` and `LID` per clause (section 7) and chip level `CONF_OUT`, `UP_OUT`, `DONE_OUT`, `Lit_Pos`
and `CID` (section 9) after every operation, and can emit any of them as checked `.vec` columns.

Not modelled: the step from a named row back to a variable ID. `{CID, Lit_Pos}` names the
winning row, but turning that row into the VID it stores needs a read operation, since the array
has no other path from a row to its stored key.

**No full chip wrapper exists in this repo yet.** `BCP_Top_4x16_4det_with_defs.cdl` stops at the
submodule plus four determiners, so the full chip vec has no DUT to run against until a wrapper
adding the decoder and `CombTree_2lvl` is generated. See section 10.

## 0. Requirements

- Your computer can run C++
- Python 3 (also shipped with the command line tools) for the `.vec` to markdown report.

No other dependencies. The MiniSAT solver is bundled in `minisat/`, so there is nothing to download.

## 1. Build and run

```bash
make                                  # compiles the bundled minisat + the three example ops programs
./examples/expanded_all_ops           # writes examples/out_results.txt + examples/out_checked.vec
./examples/determiner_ops             # writes examples/out_determiner_results.txt
                                      #    + examples/out_determiner.vec
./examples/fullchip_ops               # writes examples/out_fullchip_results.txt
                                      #    + examples/out_fullchip.vec
make examples/template_ops            # build the skeleton you edit (section 2)
make examples/expanded_all_ops        # (re)build one ops program
make clean                            # remove build artifacts and generated outputs
```

`make` builds only the three worked examples. `examples/template_ops.cpp` is built on demand, so a
half finished edit in it never blocks the rest of the project from compiling.

The build uses `-std=gnu++98` because minisat is C++98 era code; clang in c++11 mode rejects its
`"%4"PRIi64` style literals. The Makefile compiles `minisat/minisat/core/Solver.cc` and
`minisat/minisat/utils/System.cc` from the bundled copy (override the location with `MINISAT=...`).

## 2. Writing an ops program to generate a new .vec

**Start with `examples/template_ops.cpp`.** It carries the smallest run that produces a chip level
`UP` verdict, 18 operations, with every operation documented in one comment block at the bottom of
the file. Three steps:

**Step 1. Replace the operations** with your own, or keep them as a starting point:

```cpp
    goldenmodel::GoldenModelRun run(16, 4);

    run.write(0, 1, 'x', 0);
    run.write(1, 1, 'x', 0);      // rows 0 and 1 share vid 1
    ...                           // 16 writes: every row must be written
    run.search(1, '0');           // one search falsifies both shared rows
    run.search(2, '0');

    if (run.emitResults(results_path) != 0) return 1;
```

Why 18 is the floor:

- all 16 rows have to be written before any tree column stops being `-` (section 9);
- values have to arrive through `search` rather than through addressed writes, or the minisat
  cross checks are skipped and the emitted values are ungrounded;
- the other three clauses need no operations at all. A clause whose four literals are all
  unassigned reports `CONF=UP=DONE=0`, so it can neither conflict nor compete as a unit clause;
  the `x` load leaves them inert for free;
- that leaves three literals of the scenario clause to falsify, which is three searches, or two
  once rows 0 and 1 share a vid.

Being minimal costs coverage: no clause is ever satisfied, so chip level `DONE` never reaches 1,
there is no conflict, and only one clause is ever unit. `examples/fullchip_ops.cpp` covers those
in 33 operations.

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

The template calls `emitCheckedVecFullChip`, since its scenario exists to exercise the tree
columns. Swap that one line for the DUT you intend to simulate:

| Emitter | Target | Columns |
|---|---|---|
| `emitCheckedVec` | bare `Submodule_4x16` | 16 one hot `ADDR_IN` in; `Q_Val` / `Q_Pol` / read path out |
| `emitCheckedVecWithDeterminer` | `BCP_Top_4x16_4det` | the above plus 5 columns per determiner |
| `emitCheckedVecFullChip` | decoder + array + 4 determiners + tree | binary `ADDR_IN[3:0]` in place of the 16 one hot pins; the above plus `D_out[15:0]` and 7 tree columns |

Every `.vec` column is named after the hardware pin it drives or checks, since Cadence binds
columns by the name on the `vname` line. Per determiner `d` those names are `CONF<d>`, `UP<d>`,
`DONE<d>` and `LID<2d+1>` / `LID<2d>`; the chip level tree columns are `CONF_OUT`, `UP_OUT`,
`DONE_OUT`, `Lit_Pos<1>` / `Lit_Pos<0>` and `CID<1>` / `CID<0>`. The `_OUT` suffix is what keeps the
tree verdict distinct from determiner 0. Note that the decoder's address bits are also called
`ADDR_IN<*>`, the same prefix the other two flavours use for their 16 one hot pins, so the only way
to tell a binary `ADDR_IN` from a one hot one is whether the vec carries `D_out<0>`.

The determiner and tree verdicts are computed whichever you pick and always appear in the results
log; the choice only changes which columns land in the `.vec`. The full chip flavour needs exactly
16 rows and 4 determiners, since `CombTree_2lvl` is a fixed two level tree and the decoder is a
fixed 4 bit decoder, and it uses a 12 ns slot per operation instead of 10 ns (section 8).

### Worked examples to copy from

- `examples/expanded_all_ops.cpp` is a three operation minimum: write, search, read.
- `examples/determiner_ops.cpp` drives eight clause scenarios and documents the expected
  `CONF` / `UP` / `DONE` / `Lit_Pos` for each, taken from the Determiner's own reference vectors.
  Useful as a correctness reference when you are unsure what a verdict should be.
- `examples/fullchip_ops.cpp` writes all 16 rows and then drives every chip level case through
  searches: all clauses satisfied, one clause unit, two clauses unit at once, and a conflict that
  must outrank a unit. It writes all 16 rows on purpose, unlike `determiner_ops.cpp`: with rows 4
  to 15 never written, determiners 1 to 3 report `-` and every tree column would be `-` for the
  whole run.

Build any of them the same way, with `make examples/<name>`.

### 2.1 The 2 bit Val encoding

The three literal values are stored in two SRAM cells per row, driven as `Val_IN<1> Val_IN<0>` and
read back on the `Q_Val` nodes:

| `val` character | `{Val<1>, Val<0>}` | meaning |
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
tree_conf -
tree_up -
tree_done -
tree_lid --
tree_cid --
```

- `q_val` has 2n characters; character `2r` is the MSB storage node `Q_Val[2r]` of row r and `2r+1`
  the LSB, matching the generated netlist wiring (`Q[2r]` maps to `BL[1]`). `q_pol` has n
  characters. `-` means the row was never addressed written, so its power up state is indeterminate
  and Cadence must not compare it.
- `conf_det` / `up_det` / `done_det` have one character per determiner and `lid_det` has two, MSB
  first. `-` means that clause contains at least one never written row, so no verdict is defined.
  These four lines appear whichever vec flavour you emit, since the verdict is computed either way.
- `tree_conf` / `tree_up` / `tree_done` are one character each and `tree_lid` / `tree_cid` two,
  MSB first: the chip level verdict out of the Combining Tree. All five are `-` when any of the
  four determiners is `-`, since the tree cannot have a defined output when one of its inputs does
  not. `tree_cid` names the winning clause and `tree_lid` the literal within it, so together they
  name a row; both are only meaningful when `tree_up` is 1.
- `matched rows=` appears after every search (informational; `CAM_ML` is not a port, so match
  correctness is verified indirectly through the Val write it causes).
- `read vid= val= pol=` appears only after read operations and maps onto `VID_OUT<k-1>` down to `VID_OUT<0>` (MSB
  first), `Val_OUT<1> Val_OUT<0>`, `Pol_OUT` on the SenseEN strobe line. Reading a never written row
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
- per row VID is recovered from the `ADDR_IN<*>` and `VID_IN<*>` input columns of write operations
  whose `WE_CAM` is asserted (a `wecam=0` write leaves the row's VID unchanged);
- the settled `Q_Val` / `Q_Pol` for an operation are read from the first data line of the next
  operation's block, which by the emitter's strobe policy (section 8) carries the previous
  operation's settled snapshot; this uniformly captures both a write's own commit and a search's
  deferred commit. `-` cells are passed through verbatim (never written / do not compare).

If the vec carries determiner columns, a second table per operation is appended with **Determiner,
Rows, CONF, UP, DONE, LID**. Those values come from the operation's own **last** data line, its
settle row (section 7), not from the next operation's block. On a vec without determiner columns
the script behaves exactly as before.

If the vec carries tree columns, a third table is appended with **CONF_OUT, UP_OUT, DONE_OUT, CID,
Lit_Pos, Row named**, read from the operation's last data line, which on a full chip vec is the
tree settle row. The last cell is the arithmetic the tree exists to enable:
`CID * 4 + Lit_Pos` is the row the chip is pointing at, filled in only when `UP_OUT` is 1. On a
full chip vec the `ADDR_IN<*>` columns are 4 binary address bits rather than 16 one hot pins, so the
per row VID column is recovered by decoding them the same way the `Decoder_4to16` cell does. The
script tells the two apart by looking for `D_out<0>`, which only a full chip vec carries.

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
|-- examples/template_ops.cpp         START HERE: minimal 18 op scenario, all three ops documented
|-- examples/expanded_all_ops.cpp     minimal worked example: write, search, read
|-- examples/determiner_ops.cpp       determiner scenarios with known good expected verdicts
|-- examples/fullchip_ops.cpp         chip level scenarios: satisfied, unit, two unit, conflict
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

| Operation | Control rows | Commit | Determiner settle | Tree settle |
|---|---|---|---|---|
| write | +0 precharge, +5 write driver, +6 wordline on, +7 off | +6 | +8 | +10 |
| search | +0 SL precharge low, +2 ML precharge high, +5 drivers, +7 wordline enable | +7 | +9 | +11 |
| read | +0 bitline precharge, +5 ReadEN, +6 wordline, +10 SenseEN strobe | none | none | none |

The slot is 10 ns for the memory only and determiner flavours, which have no tree settle row, and
12 ns for the full chip flavour. A search commits at +7, so its tree row lands at +11 and cannot
fit a 10 ns slot.

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
VDD = 1.1) now also apply to the determiner, decoder and tree columns. They were chosen for SRAM
storage nodes; confirm they suit standard cell outputs before trusting a pass.

**Where the 2 ns increments come from.** They are inherited from the determiner offset, not
measured. `Determiner/reports` gives Design Compiler arrival times of 0.087 ns for `DET_StageOne`,
0.106 ns for `DET_StageTwo` and 0.132 ns for `DET_StageThree`, so `DET_Hier` is roughly 0.33 ns end
to end, and `CombTree_2lvl` has no timing report at all but is structurally comparable. Those
numbers are 65nm typical corner with zero wire load, so both settle offsets are deliberately
generous. If a clean run shows the margin is real, the two settle rows can be collapsed into one at
commit + 2 and the slot returned to 10 ns.

## 9. The Combining Tree model

`CombTree_2lvl` reduces the four determiner verdicts to one chip level answer plus `LID_out[1:0]`
and `CID_out[1:0]`. `evaluateCombiningTreeNode` in `golden_model_core.cpp` transcribes the
reconstructed node in `Combining_Tree_2lvl/verilog_code/combining_tree_verilog.v` gate for gate,
internal nets included, and `evaluateCombiningTree` wires three of them the way the module does:
determiners 0 and 1, determiners 2 and 3, then those two results.

For legal determiner states the node equations reduce to something readable:

| Output | Reduces to | Meaning |
|---|---|---|
| `CONF` | `CONF_L or CONF_R` | a conflict in any clause is a chip level conflict |
| `DONE` | `DONE_L and DONE_R` | every clause has a true literal |
| `UP` | `(UP_L or UP_R) and not CONF` | the paper's BACKTRACK over UP priority, in one gate |

Note that chip level `CONF` ORs across clauses while the Determiner's internal `CONF` ANDs across
literal pairs. Both are correct: a clause conflicts only when all its literals are false, but the
chip conflicts as soon as any one clause does.

`{CID_out, LID_out}` is a 4 bit row index, which is how the tree closes most of the gap between a
verdict and an actionable answer. Turning that row into the VID it stores still needs a read.

**Minisat grounding.** `verifyCombiningTreeAgainstSolver` derives the three verdict bits
independently from `Solver::value` over the rebuilt literals and warns on any disagreement, under
the same contract as the determiner check: warnings only, never a change to an emitted value. When
`UP` is set it also checks that the row named by `{CID_out, LID_out}` really is the one unassigned
literal of an otherwise false clause. That check pins `CID_out` and `LID_out` uniquely when exactly
one clause is unit. When two or more clauses are unit it can only confirm the tree named a legal
one: which of several simultaneously unit clauses wins is the tree's own arbitration and has no
minisat referent.

## 10. The Decoder model, and what is still missing in hardware

`Decoder_4to16` (cell `Decoder`, `A_in[3:0]` to `D_out[15:0]`) replaces the 16 one hot `ADDR_IN`
pins with a 4 bit binary address. The model is one function, `decoderOutputText`: one asserted line
per address.

Which line an address asserts is set by `DECODER_ASSERTS_REVERSED_ONE_HOT`. The default `true` is
the reversed scheme, where address `r` asserts `D_out<15 - r>`, so `A_in = 0001` asserts `D_out<14>`
and a search asserts `D_out<15>`. Setting it to `false` gives plain one hot, address `r` asserts
`D_out<r>`. This changes which signal the vec checks, not how the row is formatted; the two match
different physical wirings of the `D_out` bus onto the wordlines. `notes.md` section 8 has the
detail.

Both new column groups are emitted MSB first, `ADDR_IN<3>` down to `ADDR_IN<0>` and `D_out<15>` down
to `D_out<0>`, so a vec row reads as the bus value written the usual way: `A_in = 0001` prints its
`D_out` group as `0000000000000010`, the 1 sitting in the `D_out<1>` column. This matches the
`Lit_Pos` and `LID` columns. `Q_Val` and `Q_Pol` keep their ascending per row order, since
those characters index storage nodes rather than a bus.

Searches drive `A_in = 0000`. A decoder with no enable always asserts exactly one line, so there is
no longer a "no row selected" state, but a search holds `WE_CAM = 0` and `SRAM_WL_mode = 0`, and
`SRAM_WL_mode = 0` selects the matchline path over the address path, so the hot line cannot reach a
wordline. `D_out` columns are `-` on the first data row of each operation, where `A_in` changes,
and asserted on every later row.
