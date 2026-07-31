#include "minisat/core/Solver.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace goldenmodel {

using Minisat::Var;
using Minisat::Lit;
using Minisat::lbool;
using Minisat::mkLit;
using Minisat::var;
using Minisat::sign;

class HwBcpSolver : public Minisat::Solver {
public:
    void ensureVariableExists(Var variable) {
        while (nVars() <= variable) newVar();
    }

    void hardwareEnqueue(Lit literal) {
        if (value(var(literal)) != Minisat::l_Undef)
            hardwareUnassign(var(literal));
        uncheckedEnqueue(literal);
    }

    void hardwareUnassign(Var variable) {
        if (value(variable) == Minisat::l_Undef) return;
        for (int position = trail.size() - 1; position >= 0; position--) {
            if (var(trail[position]) == variable) {
                polarity[variable] = sign(trail[position]);
                trail[position] = trail.last();
                trail.pop();
                if (qhead > trail.size()) qhead = trail.size();
                break;
            }
        }
        assigns[variable] = Minisat::l_Undef;
        insertVarOrder(variable);
    }

    lbool hardwareValue(Var variable) const {
        return value(variable);
    }

    void setStoredPolarity(Var variable, int polarity_bit) {
        polarity[variable] = (char)polarity_bit;
    }

    bool trailContainsVariable(Var variable) const {
        for (int position = 0; position < trail.size(); position++)
            if (var(trail[position]) == variable) return true;
        return false;
    }

    Minisat::CRef allocateClause(const Minisat::vec<Lit>& literals) {
        return ca.alloc(literals, false);
    }

    bool clauseSatisfied(Minisat::CRef clause_reference) const {
        return satisfied(ca[clause_reference]);
    }

    void freeClause(Minisat::CRef clause_reference) {
        ca.free(clause_reference);
    }
};

static const int VALBITS_FALSE = 0;
static const int VALBITS_UNKNOWN = 1;
static const int VALBITS_TRUE = 3;

static const int DETERMINER_LITERAL_COUNT = 4;
static const bool DETERMINER_READS_EVEN_NODE_AS_VAL1 = true;
static const int DETERMINER_SETTLE_OFFSET = 2;

inline lbool valbits_to_lbool(int value_bits) {
    if (value_bits == VALBITS_FALSE) return Minisat::l_False;
    if (value_bits == VALBITS_TRUE) return Minisat::l_True;
    return Minisat::l_Undef;
}

inline int lbool_to_valbits(lbool value) {
    if (value == Minisat::l_False) return VALBITS_FALSE;
    if (value == Minisat::l_True) return VALBITS_TRUE;
    return VALBITS_UNKNOWN;
}

inline char valbits_to_char(int value_bits) {
    if (value_bits == VALBITS_FALSE) return '0';
    if (value_bits == VALBITS_TRUE) return '1';
    return 'x';
}

inline int char_to_valbits(char value_character) {
    if (value_character == '0') return VALBITS_FALSE;
    if (value_character == '1') return VALBITS_TRUE;
    if (value_character == 'x') return VALBITS_UNKNOWN;
    throw std::runtime_error(std::string("val must be '0', '1', or 'x', got '") + value_character + "'");
}

inline std::string to_string_helper(long number) {
    std::ostringstream stream;
    stream << number;
    return stream.str();
}

struct Operation {
    enum Kind { WriteOperation, SearchOperation, ReadOperation };
    Kind kind;
    int row;
    int vid;
    int value_bits;
    int pol;
    int write_enable_cam;
    int operation_number;

    Operation() : kind(ReadOperation), row(0), vid(0), value_bits(VALBITS_UNKNOWN), pol(0),
                  write_enable_cam(1), operation_number(0) {}

    std::string echoText() const {
        std::ostringstream text;
        if (kind == WriteOperation) {
            text << "write row=" << row << " vid=" << vid
                 << " val=" << valbits_to_char(value_bits) << " pol=" << pol;
            if (write_enable_cam == 0) text << " wecam=0";
        } else if (kind == SearchOperation) {
            text << "search vid=" << vid << " val=" << valbits_to_char(value_bits);
        } else {
            text << "read row=" << row;
        }
        return text.str();
    }
};

struct OperationResult {
    std::string echo_text;
    std::string q_val_text;
    std::string q_pol_text;
    std::string conf_det_text;
    std::string up_det_text;
    std::string done_det_text;
    std::string lid_det_text;
    bool has_read;
    std::string read_vid_text;
    std::string read_val_text;
    std::string read_pol_text;
    bool has_matched;
    std::vector<int> matched_rows;

    OperationResult() : has_read(false), has_matched(false) {}
};

struct DeterminerLiteralState {
    int value_bit;
    int unknown_bit;

    DeterminerLiteralState() : value_bit(0), unknown_bit(0) {}
};

struct DeterminerPairVerdict {
    int conflict_bit;
    int unit_bit;
    int done_bit;
    int position_bit;

    DeterminerPairVerdict() : conflict_bit(0), unit_bit(0), done_bit(0), position_bit(0) {}
};

struct DeterminerVerdict {
    bool comparable;
    int conflict_bit;
    int unit_bit;
    int done_bit;
    int position;

    DeterminerVerdict() : comparable(false), conflict_bit(0), unit_bit(0), done_bit(0),
                          position(0) {}
};

class OperationModel {
public:
    OperationModel(int row_count, int vid_bit_count)
        : row_count_(row_count), vid_bit_count_(vid_bit_count),
          determiner_count_(row_count / DETERMINER_LITERAL_COUNT),
          row_vid_(row_count, 0), row_pol_(row_count, 0),
          row_val_(row_count, Minisat::l_Undef),
          row_written_(row_count, false), row_vid_written_(row_count, false) {
        if (row_count % DETERMINER_LITERAL_COUNT != 0)
            throw std::runtime_error("row count must be a multiple of the determiner literal count");
    }

    int rowCount() const { return row_count_; }
    int vidBitCount() const { return vid_bit_count_; }
    int determinerCount() const { return determiner_count_; }
    HwBcpSolver& solver() { return solver_; }
    lbool rowValue(int row) const { return row_val_[row]; }
    int rowVid(int row) const { return row_vid_[row]; }
    int rowPol(int row) const { return row_pol_[row]; }
    bool rowWritten(int row) const { return row_written_[row]; }

    OperationResult apply(const Operation& operation) {
        OperationResult result;
        result.echo_text = operation.echoText();
        if (operation.kind == Operation::WriteOperation) applyWrite(operation, result);
        else if (operation.kind == Operation::SearchOperation) applySearch(operation, result);
        else applyRead(operation, result);
        result.q_val_text = qValText();
        result.q_pol_text = qPolText();

        std::vector<DeterminerVerdict> verdicts;
        for (int determiner = 0; determiner < determiner_count_; determiner++)
            verdicts.push_back(evaluateDeterminer(determiner));
        fillDeterminerText(verdicts, result);
        verifyDeterminersAgainstSolver(verdicts, operation.operation_number);
        return result;
    }

private:
    int row_count_;
    int vid_bit_count_;
    int determiner_count_;
    HwBcpSolver solver_;
    std::vector<int> row_vid_;
    std::vector<int> row_pol_;
    std::vector<lbool> row_val_;
    std::vector<bool> row_written_;
    std::vector<bool> row_vid_written_;

    void checkRowRange(int row, int operation_number) const {
        if (row < 0 || row >= row_count_)
            throw std::runtime_error("op " + to_string_helper(operation_number) + ": row out of range");
    }

    void checkVidRange(int vid, int operation_number) const {
        if (vid < 0 || vid >= (1 << vid_bit_count_))
            throw std::runtime_error("op " + to_string_helper(operation_number) + ": vid out of range");
    }

    void applyWrite(const Operation& operation, OperationResult& result) {
        checkRowRange(operation.row, operation.operation_number);
        checkVidRange(operation.vid, operation.operation_number);
        if (operation.write_enable_cam == 1) {
            solver_.ensureVariableExists(operation.vid);
            row_vid_[operation.row] = operation.vid;
            row_vid_written_[operation.row] = true;
            solver_.setStoredPolarity(operation.vid, operation.pol);
            solver_.setPolarity(operation.vid, operation.pol ? Minisat::l_True : Minisat::l_False);
        }
        lbool new_value = valbits_to_lbool(operation.value_bits);
        row_val_[operation.row] = new_value;
        row_pol_[operation.row] = operation.pol;
        row_written_[operation.row] = true;

        if (!(new_value == Minisat::l_Undef) && row_vid_written_[operation.row]) {
            int this_vid = row_vid_[operation.row];
            for (int other = 0; other < row_count_; other++) {
                if (other == operation.row) continue;
                if (!row_vid_written_[other] || row_vid_[other] != this_vid) continue;
                if (!row_written_[other]) continue;
                if (row_val_[other] == Minisat::l_Undef) continue;
                if (!(row_val_[other] == new_value)) {
                    fprintf(stderr, "warning: op %d: addressed write puts val=%c in row %d "
                            "but row %d shares vid=%d and holds a different concrete value; "
                            "per variable solver state cannot represent this\n",
                            operation.operation_number, valbits_to_char(operation.value_bits),
                            operation.row, other, this_vid);
                    break;
                }
            }
        }
    }

    void applySearch(const Operation& operation, OperationResult& result) {
        checkVidRange(operation.vid, operation.operation_number);
        solver_.ensureVariableExists(operation.vid);
        lbool target_value = valbits_to_lbool(operation.value_bits);
        if (target_value == Minisat::l_Undef)
            solver_.hardwareUnassign(operation.vid);
        else
            solver_.hardwareEnqueue(mkLit(operation.vid, target_value == Minisat::l_False));

        result.has_matched = true;
        for (int row = 0; row < row_count_; row++) {
            if (row_vid_written_[row] && row_vid_[row] == operation.vid) {
                row_val_[row] = solver_.hardwareValue(operation.vid);
                result.matched_rows.push_back(row);
            }
        }
    }

    void applyRead(const Operation& operation, OperationResult& result) {
        checkRowRange(operation.row, operation.operation_number);
        result.has_read = true;
        int row = operation.row;
        if (row_vid_written_[row]) {
            std::string vid_text;
            for (int bit = vid_bit_count_ - 1; bit >= 0; bit--)
                vid_text += ((row_vid_[row] >> bit) & 1) ? '1' : '0';
            result.read_vid_text = vid_text;
        } else {
            result.read_vid_text = std::string((size_t)vid_bit_count_, '-');
        }
        if (row_written_[row]) {
            int value_bits = lbool_to_valbits(row_val_[row]);
            result.read_val_text = std::string(1, ((value_bits >> 1) & 1) ? '1' : '0')
                                 + std::string(1, (value_bits & 1) ? '1' : '0');
            result.read_pol_text = std::string(1, (row_pol_[row] & 1) ? '1' : '0');
        } else {
            result.read_val_text = "--";
            result.read_pol_text = "-";
        }
    }

    DeterminerLiteralState evaluateLiteral(int row) const {
        DeterminerLiteralState state;
        int value_bits = lbool_to_valbits(row_val_[row]);
        int val1 = (value_bits >> 1) & 1;
        int val0 = value_bits & 1;
        if (!DETERMINER_READS_EVEN_NODE_AS_VAL1) {
            int swapped = val1;
            val1 = val0;
            val0 = swapped;
        }
        if (val1 == 1 && val0 == 0) {
            state.value_bit = 1;
            state.unknown_bit = 1;
            return state;
        }
        lbool stored_value = Minisat::l_Undef;
        if (val1 == 0 && val0 == 0) stored_value = Minisat::l_False;
        else if (val1 == 1 && val0 == 1) stored_value = Minisat::l_True;
        lbool literal_value = stored_value ^ (row_pol_[row] != 0);
        if (literal_value == Minisat::l_True) {
            state.value_bit = 1;
        } else if (literal_value == Minisat::l_Undef) {
            state.unknown_bit = 1;
        }
        return state;
    }

    DeterminerPairVerdict evaluatePair(const DeterminerLiteralState& first,
                                       const DeterminerLiteralState& second) const {
        DeterminerPairVerdict verdict;
        int status = (first.value_bit << 3) | (first.unknown_bit << 2)
                   | (second.value_bit << 1) | second.unknown_bit;
        if (status == 0x0) {
            verdict.conflict_bit = 1;
        } else if (status == 0x8 || status == 0x2 || status == 0x9
                || status == 0x6 || status == 0xa) {
            verdict.done_bit = 1;
        } else if (status == 0x4) {
            verdict.unit_bit = 1;
        } else if (status == 0x1) {
            verdict.unit_bit = 1;
            verdict.position_bit = 1;
        } else if (status == 0x5) {
            verdict.conflict_bit = 0;
            verdict.unit_bit = 0;
            verdict.done_bit = 0;
        } else {
            verdict.conflict_bit = 1;
            verdict.unit_bit = 1;
            verdict.done_bit = 1;
        }
        return verdict;
    }

    DeterminerVerdict evaluateDeterminer(int determiner) const {
        DeterminerVerdict verdict;
        int first_row = determiner * DETERMINER_LITERAL_COUNT;
        for (int offset = 0; offset < DETERMINER_LITERAL_COUNT; offset++)
            if (!row_written_[first_row + offset]) return verdict;
        verdict.comparable = true;

        DeterminerPairVerdict left = evaluatePair(evaluateLiteral(first_row),
                                                  evaluateLiteral(first_row + 1));
        DeterminerPairVerdict right = evaluatePair(evaluateLiteral(first_row + 2),
                                                   evaluateLiteral(first_row + 3));

        int left_not_done = (left.done_bit == 0) ? 1 : 0;
        int right_not_done = (right.done_bit == 0) ? 1 : 0;
        verdict.done_bit = left.done_bit | right.done_bit;
        verdict.conflict_bit = (left.conflict_bit & right.conflict_bit)
                             | (left.conflict_bit & left.done_bit)
                             | (right.conflict_bit & right.done_bit);
        verdict.unit_bit = (left.unit_bit & right.conflict_bit & right_not_done)
                         | (right.unit_bit & left.conflict_bit & left_not_done);
        if (verdict.unit_bit == 1) {
            int position_high = (left.unit_bit == 1) ? 0 : 1;
            int position_low = (left.unit_bit == 1) ? left.position_bit : right.position_bit;
            verdict.position = (position_high << 1) | position_low;
        }
        return verdict;
    }

    void fillDeterminerText(const std::vector<DeterminerVerdict>& verdicts,
                            OperationResult& result) const {
        for (int determiner = 0; determiner < determiner_count_; determiner++) {
            const DeterminerVerdict& verdict = verdicts[determiner];
            if (!verdict.comparable) {
                result.conf_det_text += '-';
                result.up_det_text += '-';
                result.done_det_text += '-';
                result.lid_det_text += "--";
                continue;
            }
            result.conf_det_text += verdict.conflict_bit ? '1' : '0';
            result.up_det_text += verdict.unit_bit ? '1' : '0';
            result.done_det_text += verdict.done_bit ? '1' : '0';
            result.lid_det_text += ((verdict.position >> 1) & 1) ? '1' : '0';
            result.lid_det_text += (verdict.position & 1) ? '1' : '0';
        }
    }

    void verifyDeterminersAgainstSolver(const std::vector<DeterminerVerdict>& verdicts,
                                        int operation_number) {
        for (int determiner = 0; determiner < determiner_count_; determiner++) {
            if (!verdicts[determiner].comparable) continue;
            int first_row = determiner * DETERMINER_LITERAL_COUNT;
            bool in_sync = true;
            for (int offset = 0; offset < DETERMINER_LITERAL_COUNT && in_sync; offset++) {
                int row = first_row + offset;
                if (!row_vid_written_[row]) in_sync = false;
                else if (!(row_val_[row] == solver_.hardwareValue(row_vid_[row]))) in_sync = false;
            }
            if (!in_sync) continue;

            Minisat::vec<Lit> literals;
            for (int offset = 0; offset < DETERMINER_LITERAL_COUNT; offset++) {
                int row = first_row + offset;
                literals.push(mkLit(row_vid_[row], row_pol_[row] != 0));
            }
            Minisat::CRef clause_reference = solver_.allocateClause(literals);
            int solver_done_bit = solver_.clauseSatisfied(clause_reference) ? 1 : 0;
            solver_.freeClause(clause_reference);

            if (solver_done_bit != verdicts[determiner].done_bit)
                fprintf(stderr, "warning: op %d: determiner %d reports DONE=%d but the minisat "
                        "Solver::satisfied path reports %d for the same clause\n",
                        operation_number, determiner, verdicts[determiner].done_bit,
                        solver_done_bit);
        }
    }

    std::string qValText() const {
        std::string text;
        for (int row = 0; row < row_count_; row++) {
            if (!row_written_[row]) {
                text += "--";
            } else {
                int value_bits = lbool_to_valbits(row_val_[row]);
                text += ((value_bits >> 1) & 1) ? '1' : '0';
                text += (value_bits & 1) ? '1' : '0';
            }
        }
        return text;
    }

    std::string qPolText() const {
        std::string text;
        for (int row = 0; row < row_count_; row++) {
            if (!row_written_[row]) text += '-';
            else text += (row_pol_[row] & 1) ? '1' : '0';
        }
        return text;
    }
};

struct TimedControlRow {
    int offset;
    const char* control_bits;
};

static const TimedControlRow WRITE_CONTROL_ROWS[4] = {
    {0, "0 1 1 0 0 0 0 0 1 0"},
    {5, "0 1 1 1 0 0 1 0 1 1"},
    {6, "0 1 1 1 0 1 1 1 1 1"},
    {7, "0 1 1 1 0 0 0 0 1 1"},
};
static const TimedControlRow WRITE_CONTROL_ROWS_AFTER_READ[4] = {
    {2, "0 1 1 0 0 0 0 0 1 0"},
    {5, "0 1 1 1 0 0 1 0 1 1"},
    {6, "0 1 1 1 0 1 1 1 1 1"},
    {7, "0 1 1 1 0 0 0 0 1 1"},
};
static const TimedControlRow SEARCH_CONTROL_ROWS[4] = {
    {0, "0 1 1 1 1 0 0 0 0 0"},
    {2, "0 1 0 1 1 0 0 0 0 0"},
    {5, "0 1 1 1 0 0 1 0 0 1"},
    {7, "0 1 1 1 0 0 1 1 0 1"},
};
static const TimedControlRow SEARCH_CONTROL_ROWS_AFTER_READ[4] = {
    {2, "0 1 1 1 1 0 0 0 0 0"},
    {3, "0 1 0 1 1 0 0 0 0 0"},
    {5, "0 1 1 1 0 0 1 0 0 1"},
    {7, "0 1 1 1 0 0 1 1 0 1"},
};
static const TimedControlRow READ_CONTROL_ROWS[4] = {
    {0, "0 1 1 0 0 0 0 0 1 0"},
    {5, "0 0 1 1 0 0 0 0 1 1"},
    {6, "0 0 1 1 0 1 0 1 1 1"},
    {10, "1 1 1 0 0 0 0 0 1 0"},
};
static const TimedControlRow READ_CONTROL_ROWS_AFTER_READ[4] = {
    {2, "0 1 1 0 0 0 0 0 1 0"},
    {5, "0 0 1 1 0 0 0 0 1 1"},
    {6, "0 0 1 1 0 1 0 1 1 1"},
    {10, "1 1 1 0 0 0 0 0 1 0"},
};
static const int OPERATION_PERIOD = 10;
static const int WRITE_COMMIT_OFFSET = 6;
static const int SEARCH_COMMIT_OFFSET = 7;
static const int READ_SENSE_OFFSET = 10;
static const int WE_CAM_CONTROL_POSITION = 5;

struct OutputColumnTexts {
    std::string q_val_text;
    std::string q_pol_text;
    std::string conf_det_text;
    std::string up_det_text;
    std::string done_det_text;
    std::string lid_det_text;
    bool assert_read_outputs;
    bool include_determiner;

    OutputColumnTexts() : assert_read_outputs(false), include_determiner(false) {}
};

class VecEmitter {
public:
    VecEmitter(int row_count, int vid_bit_count)
        : row_count_(row_count), vid_bit_count_(vid_bit_count),
          determiner_count_(row_count / DETERMINER_LITERAL_COUNT) {}

    void emit(std::ostream& output, const std::vector<Operation>& operations,
              const std::vector<OperationResult>& results, bool include_determiner) const {
        emitHeader(output, include_determiner);

        std::string blank_q_val((size_t)(2 * row_count_), '-');
        std::string blank_q_pol((size_t)row_count_, '-');
        std::string blank_det((size_t)determiner_count_, '-');
        std::string blank_lid_det((size_t)(2 * determiner_count_), '-');
        std::string previous_q_val = blank_q_val;
        std::string previous_q_pol = blank_q_pol;
        std::string previous_conf_det = blank_det;
        std::string previous_up_det = blank_det;
        std::string previous_done_det = blank_det;
        std::string previous_lid_det = blank_lid_det;
        bool previous_was_read = false;
        int time_cursor = 0;

        for (size_t position = 0; position < operations.size(); position++) {
            const Operation& operation = operations[position];
            const OperationResult& result = results[position];
            output << "; " << result.echo_text << "\n";

            const TimedControlRow* control_rows;
            int commit_offset;
            if (operation.kind == Operation::WriteOperation) {
                control_rows = previous_was_read ? WRITE_CONTROL_ROWS_AFTER_READ : WRITE_CONTROL_ROWS;
                commit_offset = WRITE_COMMIT_OFFSET;
            } else if (operation.kind == Operation::SearchOperation) {
                control_rows = previous_was_read ? SEARCH_CONTROL_ROWS_AFTER_READ : SEARCH_CONTROL_ROWS;
                commit_offset = SEARCH_COMMIT_OFFSET;
            } else {
                control_rows = previous_was_read ? READ_CONTROL_ROWS_AFTER_READ : READ_CONTROL_ROWS;
                commit_offset = -1;
            }

            std::string data_tokens = dataInputTokens(operation);
            std::string last_control_tokens;
            for (int row_position = 0; row_position < 4; row_position++) {
                int offset = control_rows[row_position].offset;
                std::string control_tokens = control_rows[row_position].control_bits;
                if (operation.kind == Operation::WriteOperation
                        && offset == WRITE_COMMIT_OFFSET && operation.write_enable_cam == 0)
                    control_tokens[2 * WE_CAM_CONTROL_POSITION] = '0';
                last_control_tokens = control_tokens;

                OutputColumnTexts texts;
                texts.include_determiner = include_determiner;
                if (operation.kind == Operation::ReadOperation) {
                    texts.q_val_text = result.q_val_text;
                    texts.q_pol_text = result.q_pol_text;
                    texts.assert_read_outputs = (offset == READ_SENSE_OFFSET);
                } else if (offset == commit_offset) {
                    texts.q_val_text = blank_q_val;
                    texts.q_pol_text = blank_q_pol;
                } else if (offset > commit_offset) {
                    texts.q_val_text = result.q_val_text;
                    texts.q_pol_text = result.q_pol_text;
                } else {
                    texts.q_val_text = previous_q_val;
                    texts.q_pol_text = previous_q_pol;
                }

                if (operation.kind == Operation::ReadOperation) {
                    texts.conf_det_text = result.conf_det_text;
                    texts.up_det_text = result.up_det_text;
                    texts.done_det_text = result.done_det_text;
                    texts.lid_det_text = result.lid_det_text;
                } else if (offset < commit_offset) {
                    texts.conf_det_text = previous_conf_det;
                    texts.up_det_text = previous_up_det;
                    texts.done_det_text = previous_done_det;
                    texts.lid_det_text = previous_lid_det;
                } else {
                    texts.conf_det_text = blank_det;
                    texts.up_det_text = blank_det;
                    texts.done_det_text = blank_det;
                    texts.lid_det_text = blank_lid_det;
                }

                output << (time_cursor + offset) << " " << control_tokens << " " << data_tokens
                       << " " << outputTokens(texts, result) << "\n";
            }

            if (include_determiner && operation.kind != Operation::ReadOperation) {
                OutputColumnTexts texts;
                texts.include_determiner = true;
                texts.q_val_text = result.q_val_text;
                texts.q_pol_text = result.q_pol_text;
                texts.conf_det_text = result.conf_det_text;
                texts.up_det_text = result.up_det_text;
                texts.done_det_text = result.done_det_text;
                texts.lid_det_text = result.lid_det_text;
                output << (time_cursor + commit_offset + DETERMINER_SETTLE_OFFSET) << " "
                       << last_control_tokens << " " << data_tokens
                       << " " << outputTokens(texts, result) << "\n";
            }

            previous_q_val = result.q_val_text;
            previous_q_pol = result.q_pol_text;
            previous_conf_det = result.conf_det_text;
            previous_up_det = result.up_det_text;
            previous_done_det = result.done_det_text;
            previous_lid_det = result.lid_det_text;
            previous_was_read = (operation.kind == Operation::ReadOperation);
            time_cursor += OPERATION_PERIOD;
            output << "\n";
        }
    }

private:
    int row_count_;
    int vid_bit_count_;
    int determiner_count_;

    void emitHeader(std::ostream& output, bool include_determiner) const {
        int input_count = 10 + vid_bit_count_ + 3 + row_count_;
        int output_count = vid_bit_count_ + 3 + 2 * row_count_ + row_count_;
        if (include_determiner) output_count += 5 * determiner_count_;

        output << "radix";
        for (int position = 0; position < input_count + output_count; position++) output << " 1";
        output << "\n";

        output << "io";
        for (int position = 0; position < input_count; position++) output << " i";
        for (int position = 0; position < output_count; position++) output << " o";
        output << "\n";

        output << "vname SenseEN ReadEN MLPRE_EN SLPRE_high SLPRE_low WE_CAM SearchEN WE_SRAM"
                  " SRAM_WL_mode BLPRE_EN";
        for (int bit = vid_bit_count_ - 1; bit >= 0; bit--) output << " VID_IN_" << bit;
        output << " Val_IN_1 Val_IN_0 Pol_IN";
        for (int row = 0; row < row_count_; row++) output << " ADDR_IN_" << row;
        for (int bit = vid_bit_count_ - 1; bit >= 0; bit--) output << " VID_OUT_" << bit;
        output << " Val_OUT_1 Val_OUT_0 Pol_OUT";
        for (int position = 0; position < 2 * row_count_; position++) output << " Q_Val_" << position;
        for (int row = 0; row < row_count_; row++) output << " Q_Pol_" << row;
        if (include_determiner) {
            for (int determiner = 0; determiner < determiner_count_; determiner++) {
                output << " CONF_det_" << determiner;
                output << " UP_det_" << determiner;
                output << " DONE_det_" << determiner;
                output << " LID_det_" << (2 * determiner + 1);
                output << " LID_det_" << (2 * determiner);
            }
        }
        output << "\n";

        output << "tunit ns\n";
        output << "slope 0.01\n";
        output << "\n";
        output << "vih 1\n";
        output << "vil 0\n";
        output << "voh 0.77\n";
        output << "vol 0.33\n";
        output << "vth 0.55\n";
        output << "\n";
        output << "; Generated by the golden model ops program (golden_model_core.cpp VecEmitter)\n";
        output << "; Output columns are golden model EXPECTED values (io o). '-' = accept any.\n";
        output << "\n";
    }

    std::string dataInputTokens(const Operation& operation) const {
        std::ostringstream tokens;
        for (int bit = vid_bit_count_ - 1; bit >= 0; bit--)
            tokens << ((operation.vid >> bit) & 1) << " ";
        int value_bits = (operation.kind == Operation::ReadOperation) ? 0 : operation.value_bits;
        tokens << ((value_bits >> 1) & 1) << " " << (value_bits & 1) << " ";
        tokens << (operation.pol & 1);
        for (int row = 0; row < row_count_; row++) {
            bool addressed = (operation.kind != Operation::SearchOperation) && (row == operation.row);
            tokens << " " << (addressed ? 1 : 0);
        }
        return tokens.str();
    }

    std::string outputTokens(const OutputColumnTexts& texts,
                             const OperationResult& result) const {
        std::ostringstream tokens;
        std::string read_characters;
        if (texts.assert_read_outputs && result.has_read)
            read_characters = result.read_vid_text + result.read_val_text + result.read_pol_text;
        else
            read_characters = std::string((size_t)(vid_bit_count_ + 3), '-');
        for (size_t position = 0; position < read_characters.size(); position++) {
            if (position > 0) tokens << " ";
            tokens << read_characters[position];
        }
        for (size_t position = 0; position < texts.q_val_text.size(); position++)
            tokens << " " << texts.q_val_text[position];
        for (size_t position = 0; position < texts.q_pol_text.size(); position++)
            tokens << " " << texts.q_pol_text[position];
        if (texts.include_determiner) {
            for (int determiner = 0; determiner < determiner_count_; determiner++) {
                tokens << " " << texts.conf_det_text[(size_t)determiner];
                tokens << " " << texts.up_det_text[(size_t)determiner];
                tokens << " " << texts.done_det_text[(size_t)determiner];
                tokens << " " << texts.lid_det_text[(size_t)(2 * determiner)];
                tokens << " " << texts.lid_det_text[(size_t)(2 * determiner + 1)];
            }
        }
        return tokens.str();
    }
};

class GoldenModelRun {
public:
    GoldenModelRun(int row_count, int vid_bit_count)
        : model_(row_count, vid_bit_count) {}

    void write(int row, int vid, char val, int pol, int wecam = 1) {
        Operation operation;
        operation.kind = Operation::WriteOperation;
        operation.row = row;
        operation.vid = vid;
        operation.value_bits = char_to_valbits(val);
        operation.pol = pol;
        operation.write_enable_cam = wecam;
        applyOperation(operation);
    }

    void search(int vid, char val) {
        Operation operation;
        operation.kind = Operation::SearchOperation;
        operation.vid = vid;
        operation.value_bits = char_to_valbits(val);
        applyOperation(operation);
    }

    void read(int row) {
        Operation operation;
        operation.kind = Operation::ReadOperation;
        operation.row = row;
        applyOperation(operation);
    }

    int emitResults(const char* path) {
        std::ofstream output(path);
        if (!output) {
            fprintf(stderr, "error: cannot open results file '%s' for writing\n", path);
            return 1;
        }
        output << "config n=" << model_.rowCount() << " k=" << model_.vidBitCount() << "\n";
        for (size_t position = 0; position < results_.size(); position++) {
            const OperationResult& result = results_[position];
            output << "\n";
            output << "op " << (position + 1) << " " << result.echo_text << "\n";
            if (result.has_matched) {
                output << "matched rows=";
                if (result.matched_rows.empty()) {
                    output << "none";
                } else {
                    for (size_t matched = 0; matched < result.matched_rows.size(); matched++) {
                        if (matched > 0) output << ",";
                        output << result.matched_rows[matched];
                    }
                }
                output << "\n";
            }
            if (result.has_read)
                output << "read vid=" << result.read_vid_text
                       << " val=" << result.read_val_text
                       << " pol=" << result.read_pol_text << "\n";
            output << "q_val " << result.q_val_text << "\n";
            output << "q_pol " << result.q_pol_text << "\n";
            output << "conf_det " << result.conf_det_text << "\n";
            output << "up_det " << result.up_det_text << "\n";
            output << "done_det " << result.done_det_text << "\n";
            output << "lid_det " << result.lid_det_text << "\n";
        }
        printf("golden model: %d operations, results written to %s\n",
               (int)results_.size(), path);
        return 0;
    }

    int emitCheckedVec(const char* path) {
        return emitVec(path, false);
    }

    int emitCheckedVecWithDeterminer(const char* path) {
        return emitVec(path, true);
    }

    OperationModel& model() { return model_; }
    const std::vector<OperationResult>& results() const { return results_; }

private:
    OperationModel model_;
    std::vector<Operation> operations_;
    std::vector<OperationResult> results_;

    int emitVec(const char* path, bool include_determiner) {
        std::ofstream output(path);
        if (!output) {
            fprintf(stderr, "error: cannot open vec file '%s' for writing\n", path);
            return 1;
        }
        VecEmitter emitter(model_.rowCount(), model_.vidBitCount());
        emitter.emit(output, operations_, results_, include_determiner);
        printf("golden model: %d operations, checked vec written to %s (end time %d ns)%s\n",
               (int)operations_.size(), path, (int)operations_.size() * OPERATION_PERIOD,
               include_determiner ? " with determiner columns" : "");
        return 0;
    }

    void applyOperation(Operation operation) {
        operation.operation_number = (int)operations_.size() + 1;
        results_.push_back(model_.apply(operation));
        operations_.push_back(operation);
    }
};

}


// This file is the complete minisat grounded golden model: behavioral model plus checked .vec
// emitter in one translation unit. Ops programs (see examples/) and selftest.cpp include it
// directly, so everything is header style with no main.
//
// HwBcpSolver subclasses the real Minisat::Solver compiled from minisat-master. It only uses
// members that Solver declares protected, so no access related change to minisat is needed:
//   * ensureVariableExists loops the real newVar() (Solver.cc:119-140) so a VID gets a live
//     variable with assigns = l_Undef, exactly like solver startup.
//   * hardwareEnqueue funnels every search driven value write through the real
//     uncheckedEnqueue (Solver.cc:486-492), whose body is assigns[var(p)] = lbool(!sign(p)).
//     The hardware allows rewriting an already assigned row, but uncheckedEnqueue asserts
//     value(p) == l_Undef, so the variable is first unassigned when needed.
//   * hardwareUnassign restates the per variable part of cancelUntil (Solver.cc:231-242):
//     save the phase into polarity[] the same way cancelUntil line 237 does, remove the literal
//     from the trail, clear assigns to l_Undef, and reinsert into the order heap. qhead is
//     clamped so the solver's internal propagation cursor stays valid. Minisat itself only
//     unassigns whole decision levels, which is why this single variable restatement exists.
//   * allocateClause / clauseSatisfied / freeClause reach the protected ClauseAllocator ca
//     (Solver.h:216) and the real Solver::satisfied (Solver.cc:222) so a clause verdict can be
//     cross checked against minisat's own code. They deliberately bypass Solver::addClause_
//     (Solver.cc:154), whose sort(ps) at line 160 would reorder literals; hardware rows never
//     move, so the row to literal mapping has to survive intact.
//
// The Determiner model. DET_Hier (Determiner/design/DET_Hier.v) evaluates one clause of
// DETERMINER_LITERAL_COUNT literals and emits CONF, UP, DONE and a 2 bit Lit_Pos. Determiner d
// covers rows 4d..4d+3 as literals A, B, C, D, the grouping the wrapper wires up in
// BCP_Top_4x16_4det_with_defs.cdl. evaluateLiteral mirrors DET_StageOne: it decodes the row's
// own stored {Val1, Val0} nodes, not the solver's global assigns, because that is what the SRAM
// presents to the logic, then folds polarity with minisat's own lbool::operator^ (SolverTypes.h),
// the identical operation Solver::value(Lit) performs at Solver.h:351. evaluatePair transcribes
// the DET_StageTwo case table and evaluateDeterminer the five DET_StageThree assignments,
// including the error propagation terms, so an illegal 10 code reaches the output as the
// CONF=UP=DONE=1 signature exactly as the gates would produce it. A determiner whose rows are not
// all written emits '-' on all five columns, since power up state is indeterminate.
//
// DETERMINER_READS_EVEN_NODE_AS_VAL1 selects which Q_Val node feeds Val1. true matches
// SRAM_Array_val.v, where Q[2r] carries BL[1] and therefore Val_IN[1]. false matches
// generate_bcp_4x16_det_wrapper.pl as built today, which taps the two nodes in the opposite
// order; setting it to false makes this model reproduce that wiring, including the illegal code
// that a stored x then produces.
//
// verifyDeterminersAgainstSolver is a validation aid that never changes an emitted value. It
// rebuilds the clause as real Lits and compares Solver::satisfied against the modelled DONE. It
// runs only when every row in the clause agrees with solver.hardwareValue for its VID, because
// satisfied reads global assigns; applyWrite deliberately never enqueues, so writes leave the two
// diverged by design and only a search resyncs them. In practice the check fires after searches.
//
// Row versus variable mapping. Minisat state is indexed by Var while hardware rows store a VID,
// and several rows may share one VID. Per row state that minisat cannot represent per row lives
// in OperationModel: row_vid_ (the CAM image), row_pol_ (per row polarity bit), row_val_ (per
// row value cache mirroring the Q_Val whitebox nodes), row_written_ / row_vid_written_ (power
// up state of a never written row is indeterminate, reported as '-'). The solver is the
// authority for search driven values: applySearch performs the assignment through
// hardwareEnqueue / hardwareUnassign and then copies solver.hardwareValue(vid), i.e. the real
// Solver::value (Solver.h:350), into every matching row. Addressed writes update only the
// addressed row because that is what the hardware wordline does; a warning is printed when an
// addressed concrete value coexists with a different concrete value in another row sharing the
// same VID, since per variable assigns cannot represent that situation. A search matches only
// rows whose VID was actually written; a never written CAM row holds a random VID on real
// silicon and no model can predict it.
//
// Value encoding. The hardware 2 bit Val {Val_1, Val_0} is 00=false, 01=x, 11=true, and 10 is
// illegal and never written. This is the encoding the Determiner reads: DET_StageOne.v decodes
// {Val1, Val0, Pol} with 11x as ONE / INV_ZERO, 00x as ZERO / INV_ONE, 01x as UNKNOWN /
// INV_UNKNOWN, and 10x as ERROR0 / ERROR1. The three legal codes are named VALBITS_FALSE,
// VALBITS_UNKNOWN, and VALBITS_TRUE, defined once at the top of this file, so nothing else
// maps a literal value onto a bit pattern. The codes are ordered false < x < true along the value
// axis, which leaves 10 as the single unused pattern and lets the Determiner treat it as a
// detectable error. Minisat lbool constants use different internal numbers, so
// valbits_to_lbool / lbool_to_valbits translate at the boundary and lbool never leaks its
// internal encoding into any text output. In q_val text, character 2*row is the MSB node
// Q_Val[2*row] and character 2*row+1 is the LSB, matching the generated netlist wiring
// (SRAM_Array_val.v wires Q[2r] to BL[1], and SRAM_WD_Val.v drives BL[1] from Val_IN[1]).
//
// GoldenModelRun is the user facing API. An ops program constructs one, calls write / search /
// read (val given as the character '0', '1', or 'x'), then emitResults (human readable log,
// never parsed by anything) and one of the two vec emitters. Operations execute immediately;
// snapshots accumulate in results_ and all emitters iterate them.
//   emitCheckedVec                targets the bare Submodule_4x16; memory columns only
//   emitCheckedVecWithDeterminer  targets BCP_Top_4x16_4det; adds 5 columns per determiner
// The determiner pins exist only on the wrapper, so the two flavours are not interchangeable.
// Determiner state is computed either way and always appears in the results log.
//
// VecEmitter owns all timing; the model itself is timing free. Recipes follow genVecFile.py
// and the expanded 4x16 vec files, each op in a 10 ns slot:
//   write  : +0 precharge, +5 write driver, +6 wordline on (commit), +7 off. With wecam=0 the
//            WE_CAM bit on the commit row is dropped so only SRAM is written.
//   search : +0 SL precharge low, +2 ML precharge high, +5 search drivers, +7 wordline enable
//            (commit).
//   read   : +0 bitline precharge with address, +5 ReadEN active (low), +6 wordline high,
//            +10 SenseEN strobe. Reads drive Val_IN=00 like the reference vec.
//   Any op that follows a read starts at +2 instead of +0 (search additionally moves its ML
//   precharge to +3) so it stays clear of the previous read's 2 ns SenseEN pulse; this
//   reproduces the line spacing of misc/4x16_Array_expanded_all_checked.vec.
// Strobe policy (same contract as ../golden_model/README.md section 5): Q_Val / Q_Pol columns
// hold the previous settled snapshot before the commit row, are all '-' on the commit row, and
// hold the new snapshot from the first row after the commit (for a search that is the next
// op's first row). Reads never change storage so their rows keep the current snapshot, and
// VID_OUT / Val_OUT / Pol_OUT are asserted only on the SenseEN row. The header carries the
// rail relative thresholds voh 0.77 / vol 0.33 / vth 0.55 (0.7, 0.3, 0.5 of VDD=1.1) matching
// ../golden_model/emit.h.
//
// Determiner settling. The determiner columns do not follow the Q_Val strobe policy above,
// because a write commits at +6 and asserts Q_Val at +7, a 1 ns window, while every stimulus
// file in Determiner/vectorFile spaces its input changes 2 ns apart and DET_Stage1_delay.vec
// exists purely to isolate edges for delay measurement. 2 ns is the only empirical statement in
// the project about how long this block needs, and the determiner's own logic delay stacks on
// top of the SRAM settling the 1 ns was tuned for. So write and search each gain one trailing
// settle row at commit + DETERMINER_SETTLE_OFFSET (+8 and +9), carrying a verbatim copy of the
// preceding row's control tokens: no control signal changes value on it, it exists only to give
// the determiner columns a place to be asserted. Determiner columns hold the previous verdict
// before the commit, are '-' from the commit row up to the settle row, and carry the new verdict
// on the settle row. Reads add no settle row since they change no storage. Both settle offsets
// stay inside the 10 ns slot, so no timing outside these columns moves, and emitCheckedVec never
// enters this branch at all.
