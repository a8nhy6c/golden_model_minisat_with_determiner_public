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

static const int COMBINING_TREE_INPUT_COUNT = 4;
static const int COMBINING_TREE_SETTLE_OFFSET = 4;
static const int DECODER_ADDRESS_BIT_COUNT = 4;
static const bool DECODER_ASSERTS_REVERSED_ONE_HOT = true;

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
    std::string d_out_text;
    std::string tree_conf_text;
    std::string tree_up_text;
    std::string tree_done_text;
    std::string tree_lid_text;
    std::string tree_cid_text;
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

struct CombiningTreeBranch {
    int conflict_bit;
    int unit_bit;
    int done_bit;
    int position;

    CombiningTreeBranch() : conflict_bit(0), unit_bit(0), done_bit(0), position(0) {}
};

struct CombiningTreeNodeVerdict {
    int conflict_bit;
    int unit_bit;
    int done_bit;
    int position;
    int select_bit;

    CombiningTreeNodeVerdict() : conflict_bit(0), unit_bit(0), done_bit(0), position(0),
                                 select_bit(0) {}
};

struct CombiningTreeVerdict {
    bool comparable;
    int conflict_bit;
    int unit_bit;
    int done_bit;
    int literal_position;
    int clause_position;

    CombiningTreeVerdict() : comparable(false), conflict_bit(0), unit_bit(0), done_bit(0),
                             literal_position(0), clause_position(0) {}
};

inline CombiningTreeNodeVerdict evaluateCombiningTreeNode(const CombiningTreeBranch& left,
                                                          const CombiningTreeBranch& right) {
    int position_left_high = (left.position >> 1) & 1;
    int position_left_low = left.position & 1;
    int position_right_high = (right.position >> 1) & 1;
    int position_right_low = right.position & 1;

    int net1 = !right.unit_bit;
    int net2 = !left.unit_bit;
    int net4 = !right.conflict_bit;
    int net6 = !left.conflict_bit;
    int net7 = !right.done_bit;

    CombiningTreeNodeVerdict verdict;
    verdict.conflict_bit = !(net4 && net6);
    verdict.unit_bit = !((net1 && net2) || verdict.conflict_bit);

    int net5 = !(net7 || left.done_bit || net4);
    int net3 = !((right.conflict_bit || net2) && (net5 || net6));
    verdict.select_bit = !((net4 && net1) || net3);

    int position_high = (position_right_high && verdict.select_bit)
                      || (position_left_high && net3);
    int position_low = (position_right_low && verdict.select_bit)
                     || (position_left_low && net3);
    verdict.position = (position_high << 1) | position_low;

    int net8 = !(right.conflict_bit || left.done_bit);
    verdict.done_bit = !(net8 || net7) || (left.conflict_bit && left.done_bit);
    return verdict;
}

inline CombiningTreeBranch branchOfDeterminer(const DeterminerVerdict& verdict) {
    CombiningTreeBranch branch;
    branch.conflict_bit = verdict.conflict_bit;
    branch.unit_bit = verdict.unit_bit;
    branch.done_bit = verdict.done_bit;
    branch.position = verdict.position;
    return branch;
}

inline CombiningTreeBranch branchOfNode(const CombiningTreeNodeVerdict& verdict) {
    CombiningTreeBranch branch;
    branch.conflict_bit = verdict.conflict_bit;
    branch.unit_bit = verdict.unit_bit;
    branch.done_bit = verdict.done_bit;
    branch.position = verdict.position;
    return branch;
}

inline CombiningTreeVerdict evaluateCombiningTree(const std::vector<DeterminerVerdict>& verdicts) {
    CombiningTreeVerdict tree_verdict;
    if ((int)verdicts.size() != COMBINING_TREE_INPUT_COUNT) return tree_verdict;
    for (int determiner = 0; determiner < COMBINING_TREE_INPUT_COUNT; determiner++)
        if (!verdicts[determiner].comparable) return tree_verdict;
    tree_verdict.comparable = true;

    CombiningTreeNodeVerdict node_01 = evaluateCombiningTreeNode(branchOfDeterminer(verdicts[0]),
                                                                 branchOfDeterminer(verdicts[1]));
    CombiningTreeNodeVerdict node_23 = evaluateCombiningTreeNode(branchOfDeterminer(verdicts[2]),
                                                                 branchOfDeterminer(verdicts[3]));
    CombiningTreeNodeVerdict node_top = evaluateCombiningTreeNode(branchOfNode(node_01),
                                                                  branchOfNode(node_23));

    tree_verdict.conflict_bit = node_top.conflict_bit;
    tree_verdict.unit_bit = node_top.unit_bit;
    tree_verdict.done_bit = node_top.done_bit;
    tree_verdict.literal_position = node_top.position;
    int clause_high = node_top.select_bit;
    int clause_low = node_top.select_bit ? node_23.select_bit : node_01.select_bit;
    tree_verdict.clause_position = (clause_high << 1) | clause_low;
    return tree_verdict;
}

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

        CombiningTreeVerdict tree_verdict = evaluateCombiningTree(verdicts);
        fillCombiningTreeText(tree_verdict, result);
        result.d_out_text = decoderOutputText(operation);
        verifyCombiningTreeAgainstSolver(tree_verdict, operation.operation_number);
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

    void fillCombiningTreeText(const CombiningTreeVerdict& tree_verdict,
                               OperationResult& result) const {
        if (!tree_verdict.comparable) {
            result.tree_conf_text = "-";
            result.tree_up_text = "-";
            result.tree_done_text = "-";
            result.tree_lid_text = "--";
            result.tree_cid_text = "--";
            return;
        }
        result.tree_conf_text = tree_verdict.conflict_bit ? "1" : "0";
        result.tree_up_text = tree_verdict.unit_bit ? "1" : "0";
        result.tree_done_text = tree_verdict.done_bit ? "1" : "0";
        result.tree_lid_text = std::string(1, ((tree_verdict.literal_position >> 1) & 1) ? '1' : '0')
                             + std::string(1, (tree_verdict.literal_position & 1) ? '1' : '0');
        result.tree_cid_text = std::string(1, ((tree_verdict.clause_position >> 1) & 1) ? '1' : '0')
                             + std::string(1, (tree_verdict.clause_position & 1) ? '1' : '0');
    }

    std::string decoderOutputText(const Operation& operation) const {
        int addressed_row = (operation.kind == Operation::SearchOperation) ? 0 : operation.row;
        int asserted_line = DECODER_ASSERTS_REVERSED_ONE_HOT
                            ? (row_count_ - 1 - addressed_row) : addressed_row;
        std::string text;
        for (int line = row_count_ - 1; line >= 0; line--)
            text += (line == asserted_line) ? '1' : '0';
        return text;
    }

    lbool solverLiteralValue(int row) const {
        return solver_.hardwareValue(row_vid_[row]) ^ (row_pol_[row] != 0);
    }

    bool clauseAgreesWithSolver(int determiner) const {
        int first_row = determiner * DETERMINER_LITERAL_COUNT;
        for (int offset = 0; offset < DETERMINER_LITERAL_COUNT; offset++) {
            int row = first_row + offset;
            if (!row_vid_written_[row]) return false;
            if (!(row_val_[row] == solver_.hardwareValue(row_vid_[row]))) return false;
        }
        return true;
    }

    void verifyCombiningTreeAgainstSolver(const CombiningTreeVerdict& tree_verdict,
                                          int operation_number) const {
        if (!tree_verdict.comparable) return;
        if (determiner_count_ != COMBINING_TREE_INPUT_COUNT) return;
        for (int determiner = 0; determiner < determiner_count_; determiner++)
            if (!clauseAgreesWithSolver(determiner)) return;

        bool any_conflicting = false;
        bool any_unit = false;
        bool all_satisfied = true;
        for (int determiner = 0; determiner < determiner_count_; determiner++) {
            int first_row = determiner * DETERMINER_LITERAL_COUNT;
            int true_count = 0;
            int unassigned_count = 0;
            for (int offset = 0; offset < DETERMINER_LITERAL_COUNT; offset++) {
                lbool literal_value = solverLiteralValue(first_row + offset);
                if (literal_value == Minisat::l_True) true_count++;
                else if (literal_value == Minisat::l_Undef) unassigned_count++;
            }
            if (true_count > 0) continue;
            all_satisfied = false;
            if (unassigned_count == 0) any_conflicting = true;
            else if (unassigned_count == 1) any_unit = true;
        }

        int expected_conflict_bit = any_conflicting ? 1 : 0;
        int expected_done_bit = all_satisfied ? 1 : 0;
        int expected_unit_bit = (!any_conflicting && any_unit) ? 1 : 0;

        if (tree_verdict.conflict_bit != expected_conflict_bit)
            fprintf(stderr, "warning: op %d: combining tree reports CONF=%d but minisat sees "
                    "%d clauses with every literal false\n", operation_number,
                    tree_verdict.conflict_bit, expected_conflict_bit);
        if (tree_verdict.done_bit != expected_done_bit)
            fprintf(stderr, "warning: op %d: combining tree reports DONE=%d but the minisat "
                    "Solver::value path reports %d for all clauses satisfied\n",
                    operation_number, tree_verdict.done_bit, expected_done_bit);
        if (tree_verdict.unit_bit != expected_unit_bit)
            fprintf(stderr, "warning: op %d: combining tree reports UP=%d but minisat reports "
                    "%d for a unit clause with no conflict present\n", operation_number,
                    tree_verdict.unit_bit, expected_unit_bit);

        if (tree_verdict.unit_bit != 1) return;

        int first_row = tree_verdict.clause_position * DETERMINER_LITERAL_COUNT;
        int named_row = first_row + tree_verdict.literal_position;
        bool siblings_all_false = true;
        for (int offset = 0; offset < DETERMINER_LITERAL_COUNT; offset++) {
            int other_row = first_row + offset;
            if (other_row == named_row) continue;
            if (!(solverLiteralValue(other_row) == Minisat::l_False)) siblings_all_false = false;
        }
        if (!(solverLiteralValue(named_row) == Minisat::l_Undef) || !siblings_all_false)
            fprintf(stderr, "warning: op %d: combining tree reports UP naming CID_out=%d "
                    "Lit_Pos=%d, that is row %d, but minisat does not see that row as the one "
                    "unassigned literal of an otherwise false clause\n", operation_number,
                    tree_verdict.clause_position, tree_verdict.literal_position, named_row);
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
static const int FULL_CHIP_OPERATION_PERIOD = 12;
static const int WRITE_COMMIT_OFFSET = 6;
static const int SEARCH_COMMIT_OFFSET = 7;
static const int READ_SENSE_OFFSET = 10;
static const int WE_CAM_CONTROL_POSITION = 5;

enum VecFlavour {
    MemoryOnlyVec,
    DeterminerVec,
    FullChipVec
};

struct OutputColumnTexts {
    std::string q_val_text;
    std::string q_pol_text;
    std::string conf_det_text;
    std::string up_det_text;
    std::string done_det_text;
    std::string lid_det_text;
    std::string d_out_text;
    std::string tree_conf_text;
    std::string tree_up_text;
    std::string tree_done_text;
    std::string tree_lid_text;
    std::string tree_cid_text;
    bool assert_read_outputs;
    bool include_determiner;
    bool include_full_chip;

    OutputColumnTexts() : assert_read_outputs(false), include_determiner(false),
                          include_full_chip(false) {}
};

class VecEmitter {
public:
    VecEmitter(int row_count, int vid_bit_count)
        : row_count_(row_count), vid_bit_count_(vid_bit_count),
          determiner_count_(row_count / DETERMINER_LITERAL_COUNT) {}

    void emit(std::ostream& output, const std::vector<Operation>& operations,
              const std::vector<OperationResult>& results, VecFlavour flavour) const {
        bool include_determiner = (flavour != MemoryOnlyVec);
        bool include_full_chip = (flavour == FullChipVec);
        int operation_period = include_full_chip ? FULL_CHIP_OPERATION_PERIOD : OPERATION_PERIOD;
        emitHeader(output, flavour);

        std::string blank_q_val((size_t)(2 * row_count_), '-');
        std::string blank_q_pol((size_t)row_count_, '-');
        std::string blank_det((size_t)determiner_count_, '-');
        std::string blank_lid_det((size_t)(2 * determiner_count_), '-');
        std::string blank_d_out((size_t)row_count_, '-');
        std::string previous_q_val = blank_q_val;
        std::string previous_q_pol = blank_q_pol;
        std::string previous_conf_det = blank_det;
        std::string previous_up_det = blank_det;
        std::string previous_done_det = blank_det;
        std::string previous_lid_det = blank_lid_det;
        std::string previous_tree_conf = "-";
        std::string previous_tree_up = "-";
        std::string previous_tree_done = "-";
        std::string previous_tree_lid = "--";
        std::string previous_tree_cid = "--";
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

            std::string data_tokens = dataInputTokens(operation, flavour);
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
                texts.include_full_chip = include_full_chip;
                texts.d_out_text = (row_position == 0) ? blank_d_out : result.d_out_text;
                if (operation.kind == Operation::ReadOperation) {
                    texts.tree_conf_text = result.tree_conf_text;
                    texts.tree_up_text = result.tree_up_text;
                    texts.tree_done_text = result.tree_done_text;
                    texts.tree_lid_text = result.tree_lid_text;
                    texts.tree_cid_text = result.tree_cid_text;
                } else if (offset < commit_offset) {
                    texts.tree_conf_text = previous_tree_conf;
                    texts.tree_up_text = previous_tree_up;
                    texts.tree_done_text = previous_tree_done;
                    texts.tree_lid_text = previous_tree_lid;
                    texts.tree_cid_text = previous_tree_cid;
                } else {
                    texts.tree_conf_text = "-";
                    texts.tree_up_text = "-";
                    texts.tree_done_text = "-";
                    texts.tree_lid_text = "--";
                    texts.tree_cid_text = "--";
                }
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
                texts.include_full_chip = include_full_chip;
                texts.q_val_text = result.q_val_text;
                texts.q_pol_text = result.q_pol_text;
                texts.conf_det_text = result.conf_det_text;
                texts.up_det_text = result.up_det_text;
                texts.done_det_text = result.done_det_text;
                texts.lid_det_text = result.lid_det_text;
                texts.d_out_text = result.d_out_text;
                texts.tree_conf_text = "-";
                texts.tree_up_text = "-";
                texts.tree_done_text = "-";
                texts.tree_lid_text = "--";
                texts.tree_cid_text = "--";
                output << (time_cursor + commit_offset + DETERMINER_SETTLE_OFFSET) << " "
                       << last_control_tokens << " " << data_tokens
                       << " " << outputTokens(texts, result) << "\n";
            }

            if (include_full_chip && operation.kind != Operation::ReadOperation) {
                OutputColumnTexts texts;
                texts.include_determiner = true;
                texts.include_full_chip = true;
                texts.q_val_text = result.q_val_text;
                texts.q_pol_text = result.q_pol_text;
                texts.conf_det_text = result.conf_det_text;
                texts.up_det_text = result.up_det_text;
                texts.done_det_text = result.done_det_text;
                texts.lid_det_text = result.lid_det_text;
                texts.d_out_text = result.d_out_text;
                texts.tree_conf_text = result.tree_conf_text;
                texts.tree_up_text = result.tree_up_text;
                texts.tree_done_text = result.tree_done_text;
                texts.tree_lid_text = result.tree_lid_text;
                texts.tree_cid_text = result.tree_cid_text;
                output << (time_cursor + commit_offset + COMBINING_TREE_SETTLE_OFFSET) << " "
                       << last_control_tokens << " " << data_tokens
                       << " " << outputTokens(texts, result) << "\n";
            }

            previous_q_val = result.q_val_text;
            previous_q_pol = result.q_pol_text;
            previous_conf_det = result.conf_det_text;
            previous_up_det = result.up_det_text;
            previous_done_det = result.done_det_text;
            previous_lid_det = result.lid_det_text;
            previous_tree_conf = result.tree_conf_text;
            previous_tree_up = result.tree_up_text;
            previous_tree_done = result.tree_done_text;
            previous_tree_lid = result.tree_lid_text;
            previous_tree_cid = result.tree_cid_text;
            previous_was_read = (operation.kind == Operation::ReadOperation);
            time_cursor += operation_period;
            output << "\n";
        }
    }

private:
    int row_count_;
    int vid_bit_count_;
    int determiner_count_;

    void emitHeader(std::ostream& output, VecFlavour flavour) const {
        bool include_determiner = (flavour != MemoryOnlyVec);
        bool include_full_chip = (flavour == FullChipVec);
        int address_input_count = include_full_chip ? DECODER_ADDRESS_BIT_COUNT : row_count_;
        int input_count = 10 + vid_bit_count_ + 3 + address_input_count;
        int output_count = vid_bit_count_ + 3 + 2 * row_count_ + row_count_;
        if (include_determiner) output_count += 5 * determiner_count_;
        if (include_full_chip) output_count += row_count_ + 7;

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
        if (include_full_chip) {
            for (int bit = DECODER_ADDRESS_BIT_COUNT - 1; bit >= 0; bit--)
                output << " A_in_" << bit;
        } else {
            for (int row = 0; row < row_count_; row++) output << " ADDR_IN_" << row;
        }
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
        if (include_full_chip) {
            for (int row = row_count_ - 1; row >= 0; row--) output << " D_out_" << row;
            output << " CONF UP DONE LID_out_1 LID_out_0 CID_out_1 CID_out_0";
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

    std::string dataInputTokens(const Operation& operation, VecFlavour flavour) const {
        std::ostringstream tokens;
        for (int bit = vid_bit_count_ - 1; bit >= 0; bit--)
            tokens << ((operation.vid >> bit) & 1) << " ";
        int value_bits = (operation.kind == Operation::ReadOperation) ? 0 : operation.value_bits;
        tokens << ((value_bits >> 1) & 1) << " " << (value_bits & 1) << " ";
        tokens << (operation.pol & 1);
        if (flavour == FullChipVec) {
            int address = (operation.kind == Operation::SearchOperation) ? 0 : operation.row;
            for (int bit = DECODER_ADDRESS_BIT_COUNT - 1; bit >= 0; bit--)
                tokens << " " << ((address >> bit) & 1);
            return tokens.str();
        }
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
        if (texts.include_full_chip) {
            for (size_t position = 0; position < texts.d_out_text.size(); position++)
                tokens << " " << texts.d_out_text[position];
            tokens << " " << texts.tree_conf_text;
            tokens << " " << texts.tree_up_text;
            tokens << " " << texts.tree_done_text;
            tokens << " " << texts.tree_lid_text[0];
            tokens << " " << texts.tree_lid_text[1];
            tokens << " " << texts.tree_cid_text[0];
            tokens << " " << texts.tree_cid_text[1];
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
            output << "tree_conf " << result.tree_conf_text << "\n";
            output << "tree_up " << result.tree_up_text << "\n";
            output << "tree_done " << result.tree_done_text << "\n";
            output << "tree_lid " << result.tree_lid_text << "\n";
            output << "tree_cid " << result.tree_cid_text << "\n";
        }
        printf("golden model: %d operations, results written to %s\n",
               (int)results_.size(), path);
        return 0;
    }

    int emitCheckedVec(const char* path) {
        return emitVec(path, MemoryOnlyVec);
    }

    int emitCheckedVecWithDeterminer(const char* path) {
        return emitVec(path, DeterminerVec);
    }

    int emitCheckedVecFullChip(const char* path) {
        if (model_.determinerCount() != COMBINING_TREE_INPUT_COUNT) {
            fprintf(stderr, "error: the full chip vec needs exactly %d determiners, this run has "
                    "%d; CombTree_2lvl is a fixed two level tree\n",
                    COMBINING_TREE_INPUT_COUNT, model_.determinerCount());
            return 1;
        }
        if (model_.rowCount() != (1 << DECODER_ADDRESS_BIT_COUNT)) {
            fprintf(stderr, "error: the full chip vec needs %d rows to match the %d bit decoder, "
                    "this run has %d\n", 1 << DECODER_ADDRESS_BIT_COUNT,
                    DECODER_ADDRESS_BIT_COUNT, model_.rowCount());
            return 1;
        }
        return emitVec(path, FullChipVec);
    }

    OperationModel& model() { return model_; }
    const std::vector<OperationResult>& results() const { return results_; }

private:
    OperationModel model_;
    std::vector<Operation> operations_;
    std::vector<OperationResult> results_;

    int emitVec(const char* path, VecFlavour flavour) {
        std::ofstream output(path);
        if (!output) {
            fprintf(stderr, "error: cannot open vec file '%s' for writing\n", path);
            return 1;
        }
        int operation_period = (flavour == FullChipVec) ? FULL_CHIP_OPERATION_PERIOD
                                                        : OPERATION_PERIOD;
        const char* flavour_text = "";
        if (flavour == DeterminerVec) flavour_text = " with determiner columns";
        else if (flavour == FullChipVec) flavour_text = " with decoder, determiner and tree columns";
        VecEmitter emitter(model_.rowCount(), model_.vidBitCount());
        emitter.emit(output, operations_, results_, flavour);
        printf("golden model: %d operations, checked vec written to %s (end time %d ns)%s\n",
               (int)operations_.size(), path, (int)operations_.size() * operation_period,
               flavour_text);
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
// The Combining Tree model. CombTree_2lvl (Combining_Tree_2lvl/verilog_code/
// combining_tree_verilog.v) reduces the four determiner verdicts to one chip level answer.
// evaluateCombiningTreeNode transcribes the reconstructed node gate for gate, internal nets n1 to
// n8 included, and evaluateCombiningTree wires three of them the way the CombTree_2lvl module
// does: one node over determiners 0 and 1, one over 2 and 3, one over those two results, then
// CID_out[1] = select_top and CID_out[0] = select_top ? select_23 : select_01. The equations are
// left unsimplified on purpose, for the same reason the determiner stages are: illegal input
// combinations must reach the output the way the gates would produce them, not the way a tidy
// Boolean identity would. For legal determiner states they reduce to CONF = CONF_L | CONF_R
// (a conflict in any clause is a chip level conflict, the opposite of the determiner's internal
// AND over pairs), DONE = DONE_L & DONE_R, and UP = (UP_L | UP_R) & ~CONF, which is the paper's
// BACKTRACK over UP priority. If any of the four determiners is not comparable, all seven tree
// columns are '-'.
//
// verifyCombiningTreeAgainstSolver grounds the tree in minisat under the same contract as the
// determiner check: warnings only, never a change to an emitted value, and only while every row
// agrees with solver.hardwareValue for its VID. It derives the three verdict bits from
// Solver::value over the rebuilt literals, so CONF means some clause has every literal false,
// DONE means every clause has a true literal, and UP means a unit clause exists with no conflict
// anywhere. When UP is set it also checks the row named by {CID_out, LID_out}: that row must hold
// the one unassigned literal of an otherwise false clause. That last check is the only one that
// validates LID_out and CID_out rather than the verdict bits alone. It pins them uniquely when
// exactly one clause is unit; when two or more are unit it can only confirm the tree named a
// legal one, because which of several simultaneously unit clauses wins is the tree's own
// arbitration and has no minisat referent.
//
// The Decoder model. Decoder_4to16 (cell Decoder, A_in[3:0] to D_out[15:0]) replaces the 16 one
// hot ADDR_IN pins with a 4 bit binary address, so the full chip vec drives DECODER_ADDRESS_BIT_
// COUNT address bits and checks D_out as expected outputs. decoderOutputText is the whole model:
// one asserted line per address. Search operations drive A_in = 0000. A decoder with no enable
// always asserts one line, so there is no longer a "no row selected" code, but a search holds
// WE_CAM = 0 and SRAM_WL_mode = 0, and SRAM_WL_mode = 0 selects the matchline path over the
// address path, so the hot line cannot reach a wordline. D_out columns are '-' on the first data
// row of each operation, where A_in changes, and asserted on every later row, since the decoder is
// combinational and its output is only trustworthy once the address has been stable.
//
// Both new column groups are emitted MSB first, A_in_3 down to A_in_0 and D_out_15 down to
// D_out_0, so a vec row reads as the bus value written the usual way: A_in = 0001 prints its
// D_out group as 0000000000000010, the 1 in the D_out_1 column. This matches LID_out_1 LID_out_0
// and the determiner LID_det columns, and it is the reason decoderOutputText and the vname loop
// both count down. Q_Val and Q_Pol keep their ascending, per row order, since those characters
// index storage nodes rather than a bus. Nothing depends on either order beyond the two loops,
// since Cadence binds every column by the name in vname.
//
// Which D_out line an address asserts is a separate question from that print order, and it is the
// one DECODER_ASSERTS_REVERSED_ONE_HOT selects. The default true is the reversed scheme: address r
// asserts D_out_(row_count - 1 - r), so A_in = 0001 asserts D_out_14 and a search, which drives
// A_in = 0000, asserts D_out_15. Setting the flag to false gives plain one hot, where address r
// asserts D_out_r. The two differ in the signal the vec checks, not in formatting, so they are not
// interchangeable: each matches a different physical wiring of the Decoder_4to16 D_out bus onto the
// array wordlines, and no Verilog for that cell exists in the repo to settle which is right. See
// notes.md section 8.
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
//   emitCheckedVecFullChip        targets the full chip (decoder + array + 4 determiners + tree);
//                                 drives A_in[3:0] in place of the 16 ADDR_IN pins and adds
//                                 D_out[15:0] plus 7 tree columns. Requires exactly 16 rows and
//                                 4 determiners, since CombTree_2lvl is a fixed two level tree
//                                 and the decoder is a fixed 4 bit decoder.
// The determiner and tree pins exist only on their wrappers, so the three flavours are not
// interchangeable. Determiner and tree state is computed whichever flavour is emitted, and always
// appears in the results log.
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
//
// Combining Tree settling. The tree stacks its own logic delay on top of the determiner, so the
// full chip flavour adds a second trailing row at commit + COMBINING_TREE_SETTLE_OFFSET, 2 ns
// after the determiner settle row, again copying the preceding row's control tokens verbatim.
// Tree columns hold the previous verdict before the commit, are '-' from the commit row through
// the determiner settle row, and carry the new verdict on the tree settle row. A search commits at
// +7, so its tree row lands at +11 and does not fit a 10 ns slot: the full chip flavour therefore
// uses FULL_CHIP_OPERATION_PERIOD = 12 while the other two keep OPERATION_PERIOD = 10. Rows land
// at +8 then +10 for a write and +9 then +11 for a search.
//
// The 2 ns increment is inherited from the determiner offset rather than measured. For reference,
// Determiner/reports gives Design Compiler arrival times of 0.087 ns for DET_StageOne, 0.106 ns
// for DET_StageTwo and 0.132 ns for DET_StageThree, so DET_Hier is roughly 0.33 ns end to end;
// CombTree_2lvl has no timing report at all but is structurally comparable. Those numbers are
// 65nm typical corner with zero wire load, so both settle offsets are deliberately generous. If a
// clean run shows the margin is real, the two settle rows can be collapsed into one at commit + 2
// and the slot returned to 10 ns.
