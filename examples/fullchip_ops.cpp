#include "../golden_model_core.cpp"

int main(int argument_count, char** argument_values) {
    const char* results_path = argument_count > 1 ? argument_values[1] : "examples/out_fullchip_results.txt";
    const char* vec_path = argument_count > 2 ? argument_values[2] : "examples/out_fullchip.vec";

    goldenmodel::GoldenModelRun run(16, 4);

    run.write(0, 0, 'x', 0);
    run.write(1, 1, 'x', 0);
    run.write(2, 2, 'x', 0);
    run.write(3, 3, 'x', 0);
    run.write(4, 4, 'x', 0);
    run.write(5, 5, 'x', 0);
    run.write(6, 6, 'x', 0);
    run.write(7, 7, 'x', 0);
    run.write(8, 8, 'x', 0);
    run.write(9, 9, 'x', 0);
    run.write(10, 10, 'x', 0);
    run.write(11, 11, 'x', 0);
    run.write(12, 12, 'x', 0);
    run.write(13, 13, 'x', 0);
    run.write(14, 14, 'x', 0);
    run.write(15, 15, 'x', 0);

    run.search(0, '1');
    run.search(4, '1');
    run.search(8, '1');
    run.search(12, '1');

    run.search(0, '0');
    run.search(1, '0');
    run.search(2, '0');

    run.read(3);

    run.search(8, '0');
    run.search(9, '0');
    run.search(10, '0');

    run.search(4, '0');
    run.search(5, '0');
    run.search(6, '0');
    run.search(7, '0');

    run.search(4, '1');

    run.search(3, '1');

    if (run.emitResults(results_path) != 0) return 1;
    if (run.emitCheckedVecFullChip(vec_path) != 0) return 1;
    return 0;
}

/*
Full chip ops program: decoder, CAM+SRAM array, four Determiners, and the two level Combining
Tree. It targets the full chip wrapper, so the emitted vec drives A_in[3:0] instead of the 16 one
hot ADDR_IN pins and checks D_out[15:0] plus the seven tree columns on top of everything the
determiner vec already checks.

Row grouping. Determiner d covers rows 4d to 4d+3 as literals A, B, C, D, so clause 0 is rows 0 to
3, clause 1 is rows 4 to 7, clause 2 is rows 8 to 11, and clause 3 is rows 12 to 15. Every row is
written, which the determiner_ops.cpp example does not do: with rows 4 to 15 never written,
determiners 1 to 3 emit '-' and every tree column would be '-' for the whole run.

Row r stores vid r with polarity 0, so a literal is true exactly when its row holds '1', and
search(r, val) reaches exactly one row. That one to one mapping is what makes the expected tree
verdict readable by hand.

Values move through search rather than through addressed writes on purpose. An addressed write
updates only the row and never the solver, so row state and minisat state diverge and the cross
checks are skipped; a search resyncs them. Loading every row as 'x' agrees with a fresh solver,
where every variable is l_Undef, so the checks fire from the first operation onward.

The scenarios, in order:

1. ops 1 to 16, load. All rows unassigned. No clause is satisfied and none is conflicting, so
   CONF, UP and DONE are all 0: this is the "two or more unassigned" state that the paper never
   names and that the hardware reports as the absence of all three signals.
2. ops 17 to 20, all clauses satisfied. One true literal per clause (rows 0, 4, 8, 12), so the
   tree reports DONE=1, CONF=0, UP=0.
3. ops 21 to 23, exactly one clause unit. Clause 0 becomes rows 0, 1, 2 false with row 3 still
   unassigned, so UP=1 with CID_out naming clause 0 and LID_out naming literal position 3. This is
   the case where the minisat cross check pins CID_out and LID_out uniquely.
4. op 24, a read of row 3. Reads change no storage, add no settle row, and force the next
   operation to start at +2 so it clears the SenseEN pulse.
5. ops 25 to 27, two clauses unit at once. Clause 2 joins clause 0 as unit. UP stays 1 and the
   tree must name one of the two, which the cross check can only confirm is a legal choice: the
   tie break between simultaneously unit clauses is the tree's own arbitration and has no minisat
   referent.
6. ops 28 to 31, conflict outranks unit. Clause 1 goes all false while clauses 0 and 2 are still
   unit, so the tree must report CONF=1 and UP=0. This is the paper's BACKTRACK over UP priority,
   implemented by UP = (UP_L | UP_R) & ~CONF in each tree node.
7. op 32, recovery. Row 4 returns to true, clause 1 is satisfied again, and the two unit clauses
   reassert UP.
8. op 33, the unit literal is finally assigned. Clause 0 becomes satisfied through row 3, which is
   the assignment a real solver would have made from the UP reported in step 3.
*/
