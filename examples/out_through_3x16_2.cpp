#include "../golden_model_core.cpp"

int main(int argument_count, char** argument_values) {
    const char* results_path = argument_count > 1 ? argument_values[1] : "examples/out_through_3x16_2_results.txt";
    const char* vec_path = argument_count > 2 ? argument_values[2] : "examples/out_through_3x16_2.vec";

    goldenmodel::GoldenModelRun run(16, 3);

    run.write(0, 0, 'x', 0);
    run.write(1, 1, 'x', 0);
    run.write(2, 2, 'x', 0);
    run.write(3, 3, 'x', 0);
    run.write(4, 4, 'x', 0);
    run.write(5, 5, 'x', 0);
    run.write(6, 6, 'x', 0);
    run.write(7, 7, 'x', 0);
    run.write(8, 0, 'x', 1);
    run.write(9, 1, 'x', 1);
    run.write(10, 2, 'x', 1);
    run.write(11, 3, 'x', 1);
    run.write(12, 4, 'x', 1);
    run.write(13, 5, 'x', 1);
    run.write(14, 6, 'x', 1);
    run.write(15, 7, 'x', 1);

    run.search(1, '0');
    run.search(2, '0');
    run.search(3, '0');

    run.search(5, '0');
    run.search(6, '0');
    run.search(7, '0');

    run.search(7, '1');
    run.search(0, '1');
    run.search(2, '1');
    run.search(3, '1');

    run.search(1, '0');
    run.search(4, '1');
    run.search(5, '1');
    run.search(6, 'x');

    run.search(1, 'x');

    run.search(6, '0');
    run.search(1, '0');

    if (run.emitResults(results_path) != 0) return 1;
    if (run.emitCheckedVecFullChip(vec_path) != 0) return 1;
    return 0;
}
