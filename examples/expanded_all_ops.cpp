#include "../golden_model_core.cpp"

int main(int argument_count, char** argument_values) {
    const char* results_path = argument_count > 1 ? argument_values[1] : "examples/out_results.txt"; // rename .txt as you wished
    const char* vec_path = argument_count > 2 ? argument_values[2] : "examples/out_checked.vec"; // rename .vec as you wished

    goldenmodel::GoldenModelRun run(16, 4); // first argument: number of rows ; second argument : number of bits of CAM in each row


    run.write(0, 0, 'x',0);
    run.search(0, '1');
    run.read(0);



    if (run.emitResults(results_path) != 0) return 1;
    if (run.emitCheckedVec(vec_path) != 0) return 1;
    return 0;
}

// Minimal ops program template for a 16 row, 4 VID bit submodule: write a row, search for its
// VID, then read it back. Copy this file to author your own operation sequences against the
// GoldenModelRun API (write / search / read); see README.md section 2. Build and run it with:
//     make examples/expanded_all_ops

