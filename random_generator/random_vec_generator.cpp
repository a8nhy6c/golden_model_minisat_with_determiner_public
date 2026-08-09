#include "../golden_model_core.cpp"
#include <cstdlib>
#include <sstream>

static int randomIntBelow(int upper_limit) {
    return rand() % upper_limit;
}

int main() {
    int number_of_vec_files = 10;
    const char* filename_base = "random_generator/out_random";
    int filename_start_number = 1;
    unsigned int random_seed = 982;

    const int row_count = 16;
    const int vid_bit_count = 3;
    const int vid_count = 1 << vid_bit_count;
    const int maximum_appearances_per_vid = 3;
    const char legal_value_characters[3] = {'0', '1', 'x'};

    for (int file_index = 0; file_index < number_of_vec_files; file_index++) {
        srand(random_seed + (unsigned int)(filename_start_number + file_index));

        std::ostringstream results_path;
        std::ostringstream vec_path;
        results_path << filename_base << "_" << (filename_start_number + file_index)
                     << "_results.txt";
        vec_path << filename_base << "_" << (filename_start_number + file_index) << ".vec";

        goldenmodel::GoldenModelRun run(row_count, vid_bit_count);

        int appearances_of_vid[vid_count];
        int written_vids[row_count];
        for (int vid = 0; vid < vid_count; vid++) appearances_of_vid[vid] = 0;
        for (int row = 0; row < row_count; row++) {
            int chosen_vid;
            do {
                chosen_vid = randomIntBelow(vid_count);
            } while (appearances_of_vid[chosen_vid] >= maximum_appearances_per_vid);
            appearances_of_vid[chosen_vid]++;
            written_vids[row] = chosen_vid;
            char chosen_value = legal_value_characters[randomIntBelow(3)];
            int chosen_polarity = randomIntBelow(2);
            run.write(row, chosen_vid, chosen_value, chosen_polarity);
        }

        int number_of_searches = 2 + randomIntBelow(6);
        for (int search_index = 0; search_index < number_of_searches; search_index++) {
            int searched_vid = written_vids[randomIntBelow(row_count)];
            char searched_value = legal_value_characters[randomIntBelow(3)];
            run.search(searched_vid, searched_value);
        }

        int read_order[row_count];
        for (int row = 0; row < row_count; row++) read_order[row] = row;
        for (int position = row_count - 1; position > 0; position--) {
            int swap_position = randomIntBelow(position + 1);
            int held_row = read_order[position];
            read_order[position] = read_order[swap_position];
            read_order[swap_position] = held_row;
        }
        for (int position = 0; position < row_count; position++)
            run.read(read_order[position]);

        if (run.emitResults(results_path.str().c_str()) != 0) return 1;
        if (run.emitCheckedVecFullChip(vec_path.str().c_str()) != 0) return 1;

        printf("generated %s and %s (%d searches)\n", vec_path.str().c_str(),
               results_path.str().c_str(), number_of_searches);
    }
    return 0;
}

/*
Random .vec batch generator for the 3x16 configuration (16 rows, 3 VID bits, vids 0 to 7). Each
generated scenario is random but always valid, and every file is emitted in the full chip flavour,
matching the Submodule_3x16_det_comb_dec DUT the recent simulations ran against.

The four variables at the top of main are the user controls:
-number_of_vec_files      
how many .vec files to generate in one run
- filename_base           
path prefix of every generated file, relative to where the program is run from (run it from the top of the project, like the examples)
- filename_start_number    
number appended to the base for the first file; later files count up. base "random_generator/out_random" with start 21 and 3 files gives
out_random_21.vec, out_random_22.vec, out_random_23.vec, each with a matching _results.txt log
- random_seed              
seed for the random choices. The per file seed is random_seed + file number, so the same settings always reproduce the
same batch, and any single file can be regenerated on its own by pointing filename_start_number at it with number_of_vec_files = 1

Build and run from the top of the project:
    make random_generator/random_vec_generator
    ./random_generator/random_vec_generator
*/
