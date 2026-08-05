#include "../golden_model_core.cpp"

int main(int argument_count, char** argument_values) {
    const char* results_path = argument_count > 1 ? argument_values[1] : "examples/out_template_results.txt";
    const char* vec_path = argument_count > 2 ? argument_values[2] : "examples/out_template.vec";

    goldenmodel::GoldenModelRun run(16, 4);

    run.write(0, 1, 'x', 0);
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

    run.search(1, '0');
    run.search(2, '0');

    if (run.emitResults(results_path) != 0) return 1;
    if (run.emitCheckedVecFullChip(vec_path) != 0) return 1;
    return 0;
}

/*
CHOOSING WHICH VEC TO EMIT

This file calls emitCheckedVecFullChip, since the scenario exists to exercise the tree columns.
The three flavours are not interchangeable, as each targets a different set of top level pins. The
determiner and tree verdicts are computed either way and always appear in the results log, so the
choice only changes which columns land in the vec.

    emitCheckedVec                targets the bare Submodule_4x16. Memory columns only:
                                  16 one hot ADDR_IN inputs, Q_Val / Q_Pol / read path outputs.
    emitCheckedVecWithDeterminer  targets BCP_Top_4x16_4det. Adds 5 columns per determiner.
    emitCheckedVecFullChip        targets the full chip: Decoder_4to16 + Submodule_4x16 +
                                  4 DET_Hier + CombTree_2lvl. Drives A_in[3:0] in place of the
                                  16 ADDR_IN pins, and adds D_out[15:0] plus the seven tree
                                  columns CONF, UP, DONE, LID_out[1:0], CID_out[1:0]. Needs
                                  exactly 16 rows and 4 determiners, and uses a 12 ns slot per
                                  operation instead of 10 ns to fit the tree settle row.

THE THREE OPERATIONS

    run.write(row, vid, val, pol)   addressed write of one row. Optional fifth argument wecam,
                                    default 1; pass 0 to write only the SRAM value and polarity
                                    and leave the row's stored vid alone.
    run.search(vid, val)            CAM search plus the matchline activated SRAM write. Every row
                                    holding that vid takes the value; non matching rows are
                                    untouched. A val of 'x' unassigns the variable, which is how a
                                    backtrack is modelled.
    run.read(row)                   addressed read onto VID_OUT / Val_OUT / Pol_OUT. Changes no
                                    stored state, so it produces no table in the markdown report.

val is the character '0', '1' or 'x', where 'x' means unassigned. pol is 0 for a plain literal x_i
and 1 for a negated literal NOT x_i. Any other val character, an out of range row, or an out of
range vid throws with the operation number.
*/
