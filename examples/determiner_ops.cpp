#include "../golden_model_core.cpp"

int main(int argument_count, char** argument_values) {
    const char* results_path = argument_count > 1 ? argument_values[1] : "examples/out_determiner_results.txt";
    const char* vec_path = argument_count > 2 ? argument_values[2] : "examples/out_determiner.vec";

    goldenmodel::GoldenModelRun run(16, 4);

    run.write(0, 0, 'x', 0);
    run.write(1, 1, 'x', 1);
    run.write(2, 2, 'x', 0);
    run.write(3, 3, 'x', 1);

    run.write(0, 0, '1', 0);
    run.write(1, 1, '1', 1);
    run.write(2, 2, '0', 0);
    run.write(3, 3, '1', 1);

    run.write(0, 0, '0', 0);
    run.write(1, 1, '1', 1);
    run.write(2, 2, '0', 0);
    run.write(3, 3, '1', 1);

    run.write(0, 0, 'x', 0);
    run.write(1, 1, '1', 1);
    run.write(2, 2, '0', 0);
    run.write(3, 3, '1', 1);

    run.write(0, 0, '0', 0);
    run.write(1, 1, 'x', 1);
    run.write(2, 2, '0', 0);
    run.write(3, 3, '1', 1);

    run.write(0, 0, '0', 0);
    run.write(1, 1, '1', 1);
    run.write(2, 2, 'x', 0);
    run.write(3, 3, '1', 1);

    run.write(0, 0, '0', 0);
    run.write(1, 1, '1', 1);
    run.write(2, 2, '0', 0);
    run.write(3, 3, 'x', 1);

    run.search(0, '0');
    run.search(1, '0');
    run.search(2, '0');
    run.search(3, '0');

    if (run.emitResults(results_path) != 0) return 1;
    if (run.emitCheckedVecWithDeterminer(vec_path) != 0) return 1;
    return 0;
}

// Determiner ops program. Reproduces the seven labelled scenarios of
// netlist_layout_verilog/SAT_Submodule_Verilog_Generator/Determiner/vectorFile/DET_Hier.vec in
// rows 0 to 3, which is the clause determiner 0 evaluates, plus one extra scenario that exercises
// the minisat cross check. The expected verdicts below come from that vec file's own comment
// labels and from the expected values genDET_tb.py writes into the Verilog testbench, so they are
// known good values from existing artifacts rather than from this model.
//
// Each blank line separated group above is one scenario, four writes covering rows 0 to 3. Read
// the verdict after the FOURTH write of each group, that is at ops 4, 8, 12, 16, 20, 24, 28, and
// after the last search at op 32. A determiner is only meaningful once all four of its rows hold
// data; before that its columns are '-'.
//
//   op  scenario              rows 0..3 (val, pol)                expected
//    4  load problem          x,0   x,1   x,0   x,1               conf 0  up 0  done 0  lid 00
//    8  Done                  1,0   1,1   0,0   1,1               conf 0  up 0  done 1  lid 00
//   12  Conflict              0,0   1,1   0,0   1,1               conf 1  up 0  done 0  lid 00
//   16  Unit Propagation A    x,0   1,1   0,0   1,1               conf 0  up 1  done 0  lid 00
//   20  Unit Propagation B    0,0   x,1   0,0   1,1               conf 0  up 1  done 0  lid 01
//   24  Unit Propagation C    0,0   1,1   x,0   1,1               conf 0  up 1  done 0  lid 10
//   28  Unit Propagation D    0,0   1,1   0,0   x,1               conf 0  up 1  done 0  lid 11
//   32  cross check           all four VIDs searched to 0         conf 0  up 0  done 1  lid 00
//
// Determiners 1, 2 and 3 stay '-' throughout because rows 4 to 15 are never written.
//
// Both spellings of a false literal are exercised, matching how DET_Hier.vec alternates them:
// write(row, vid, '0', 0) stores 00 with positive polarity (ZERO in DET_StageOne.v) and
// write(row, vid, '1', 1) stores 11 with negative polarity (INV_ZERO). Likewise 'x' with pol 1 is
// INV_UNKNOWN, which must still read as unknown because negating an unassigned variable leaves it
// unassigned.
//
// Scenario 8 exists because applyWrite never enqueues into the solver, so after any write a row's
// stored value and the solver's assigns for that VID diverge by design and
// verifyDeterminersAgainstSolver skips the comparison. Searching all four VIDs to 0 resyncs every
// row, which is the only state in which Solver::satisfied can be compared against the modelled
// DONE. With scenario 7's polarities still in place (0, 1, 0, 1) and all four variables false, the
// literals evaluate to false, true, false, true, so DONE is expected and minisat must agree.
// A 'warning:' line on stderr at op 32 means the cross check ran and disagreed; no warning at all
// through the whole run means every comparable clause agreed.
//
// Build and run with:
//     make examples/determiner_ops
//     ./examples/determiner_ops
