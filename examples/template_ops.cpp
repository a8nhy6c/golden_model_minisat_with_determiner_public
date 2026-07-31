#include "../golden_model_core.cpp"

int main(int argument_count, char** argument_values) {
    const char* results_path = argument_count > 1 ? argument_values[1] : "examples/out_template_results.txt";
    const char* vec_path = argument_count > 2 ? argument_values[2] : "examples/out_template.vec";

    goldenmodel::GoldenModelRun run(16, 4);

    // type your operations here
    // example:
    //run.write(0, 6, '1', 1);
    //run.search(6, '0');
    // run.read(0);

    if (run.emitResults(results_path) != 0) return 1;
    if (run.emitCheckedVecWithDeterminer(vec_path) != 0) return 1;
    return 0;
}

// Skeleton ops program. Copy this file, or just type operations into it where the marker line is,
// then build and run:
//
//     make examples/template_ops
//     ./examples/template_ops
//

