#!/usr/bin/env python3
import sys


def parse_header(lines):
    vname_tokens = None
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("vname"):
            vname_tokens = stripped.split()[1:]
            break
    if vname_tokens is None:
        raise ValueError("no 'vname' header line found in the vec file")

    column_index = {}
    for position, name in enumerate(vname_tokens):
        column_index[name] = position

    vid_bit_count = 0
    while ("VID_IN_" + str(vid_bit_count)) in column_index:
        vid_bit_count += 1
    row_count = 0
    while ("Q_Pol_" + str(row_count)) in column_index:
        row_count += 1
    if vid_bit_count == 0 or row_count == 0:
        raise ValueError("could not infer k (VID_IN_*) or n (Q_Pol_*) from vname")

    determiner_count = 0
    while ("CONF_det_" + str(determiner_count)) in column_index:
        determiner_count += 1

    address_bit_count = 0
    while ("A_in_" + str(address_bit_count)) in column_index:
        address_bit_count += 1

    has_combining_tree = "CID_out_0" in column_index

    return (column_index, vid_bit_count, row_count, determiner_count, address_bit_count,
            has_combining_tree)


def is_data_line(line):
    stripped = line.strip()
    if stripped == "" or stripped.startswith(";"):
        return False
    first = stripped.split()[0]
    try:
        float(first)
        return True
    except ValueError:
        return False


def split_into_operations(lines):
    operations = []
    current = None
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("; write") or stripped.startswith("; search") \
                or stripped.startswith("; read"):
            if current is not None:
                operations.append(current)
            kind = stripped.split()[1]
            current = {"kind": kind, "echo": stripped[2:].strip(), "data_lines": []}
        elif current is not None and is_data_line(line):
            current["data_lines"].append(stripped.split())
    if current is not None:
        operations.append(current)
    return operations


def column_value(tokens, column_index, name):
    # Each data line begins with a leading time value that the vname header does not list, so a
    # signal named at vname position p lives at data token p+1.
    return tokens[column_index[name] + 1]


def read_vid_from_write(tokens, column_index, vid_bit_count):
    bits = ""
    for bit in range(vid_bit_count - 1, -1, -1):
        bits += column_value(tokens, column_index, "VID_IN_" + str(bit))
    return bits


def snapshot_of_line(tokens, column_index, row_count):
    q_val = []
    for position in range(2 * row_count):
        q_val.append(column_value(tokens, column_index, "Q_Val_" + str(position)))
    q_pol = []
    for row in range(row_count):
        q_pol.append(column_value(tokens, column_index, "Q_Pol_" + str(row)))
    return q_val, q_pol


def determiner_snapshot_of_line(tokens, column_index, determiner_count):
    # The settle row is the last data line of a write or search block, so it is the one line whose
    # determiner columns carry that operation's verdict rather than '-' or the previous verdict.
    verdicts = []
    for determiner in range(determiner_count):
        conflict = column_value(tokens, column_index, "CONF_det_%d" % determiner)
        unit = column_value(tokens, column_index, "UP_det_%d" % determiner)
        done = column_value(tokens, column_index, "DONE_det_%d" % determiner)
        position_high = column_value(tokens, column_index, "LID_det_%d" % (2 * determiner + 1))
        position_low = column_value(tokens, column_index, "LID_det_%d" % (2 * determiner))
        verdicts.append((conflict, unit, done, position_high + position_low))
    return verdicts


def combining_tree_snapshot_of_line(tokens, column_index):
    # Read from the operation's last data line, the tree settle row, which is the one line whose
    # tree columns carry this operation's verdict rather than '-' or the previous verdict.
    conflict = column_value(tokens, column_index, "CONF")
    unit = column_value(tokens, column_index, "UP")
    done = column_value(tokens, column_index, "DONE")
    literal_position = (column_value(tokens, column_index, "LID_out_1")
                        + column_value(tokens, column_index, "LID_out_0"))
    clause_position = (column_value(tokens, column_index, "CID_out_1")
                       + column_value(tokens, column_index, "CID_out_0"))
    return conflict, unit, done, literal_position, clause_position


def addressed_row_of_operation(data_lines, column_index, row_count, address_bit_count):
    # A vec built for the bare array or the determiner wrapper carries 16 one hot ADDR_IN columns;
    # a full chip vec carries the decoder's 4 bit A_in instead, so the row has to be decoded here
    # the same way the Decoder_4to16 cell decodes it.
    if address_bit_count:
        for tokens in data_lines:
            bits = ""
            for bit in range(address_bit_count - 1, -1, -1):
                bits += column_value(tokens, column_index, "A_in_" + str(bit))
            if all(character in "01" for character in bits):
                return int(bits, 2)
        return None
    for row in range(row_count):
        for tokens in data_lines:
            if column_value(tokens, column_index, "ADDR_IN_" + str(row)) == "1":
                return row
    return None


def emit_markdown(operations, column_index, vid_bit_count, row_count, determiner_count,
                  address_bit_count=0, has_combining_tree=False):
    row_vid = ["-" * vid_bit_count for _ in range(row_count)]
    output = []
    output.append("# CAM+SRAM submodule vec expected-output report")
    output.append("")
    output.append("Reconstructed purely from the `.vec` stimulus file. Each table below is the "
                  "settled state the vec expects after one write or search operation. `Q_Val` is "
                  "the two storage nodes of a row (`Q_Val_{2r}` `Q_Val_{2r+1}`); `Q_Pol` is the "
                  "polarity node. `-` means the vec asserts nothing for that node (never written / "
                  "do not compare).")
    output.append("")
    if determiner_count:
        output.append("A second table per operation gives the Determiner verdict for each clause, "
                      "read from that operation's settle row. `Lit_Pos` is only meaningful when "
                      "`UP` is 1.")
        output.append("")
    if has_combining_tree:
        output.append("A third table gives the chip level Combining Tree verdict, read from the "
                      "tree settle row. `CID_out` names the winning clause and `LID_out` the "
                      "literal within it, so together they name a row; both are only meaningful "
                      "when `UP` is 1.")
        output.append("")
    output.append("Config: n=%d rows, k=%d VID bits, %d determiners."
                  % (row_count, vid_bit_count, determiner_count))
    output.append("")

    operation_number = 0
    for position in range(len(operations)):
        operation = operations[position]
        operation_number += 1

        if operation["kind"] == "write" and operation["data_lines"]:
            addressed_row = addressed_row_of_operation(
                operation["data_lines"], column_index, row_count, address_bit_count)
            write_enable_cam = any(
                column_value(tokens, column_index, "WE_CAM") == "1"
                for tokens in operation["data_lines"])
            if addressed_row is not None and write_enable_cam:
                row_vid[addressed_row] = read_vid_from_write(
                    operation["data_lines"][0], column_index, vid_bit_count)

        if operation["kind"] == "read":
            continue

        settled_line = None
        if position + 1 < len(operations) and operations[position + 1]["data_lines"]:
            settled_line = operations[position + 1]["data_lines"][0]
        else:
            for later in operations[position:]:
                for tokens in reversed(later["data_lines"]):
                    q_val, q_pol = snapshot_of_line(tokens, column_index, row_count)
                    if any(character != "-" for character in q_val + q_pol):
                        settled_line = tokens
                        break
                if settled_line is not None:
                    break
        if settled_line is None:
            settled_line = operation["data_lines"][-1]

        q_val, q_pol = snapshot_of_line(settled_line, column_index, row_count)

        output.append("## Op %d: %s" % (operation_number, operation["echo"]))
        output.append("")
        output.append("| Row | VID | Q_Val | Q_Pol |")
        output.append("|---|---|---|---|")
        for row in range(row_count):
            value_text = q_val[2 * row] + q_val[2 * row + 1]
            output.append("| %d | %s | %s | %s |"
                          % (row, row_vid[row], value_text, q_pol[row]))
        output.append("")

        if determiner_count and operation["data_lines"]:
            literals_per_determiner = row_count // determiner_count
            verdicts = determiner_snapshot_of_line(
                operation["data_lines"][-1], column_index, determiner_count)
            output.append("| Determiner | Rows | CONF | UP | DONE | Lit_Pos |")
            output.append("|---|---|---|---|---|---|")
            for determiner in range(determiner_count):
                conflict, unit, done, position = verdicts[determiner]
                first_row = determiner * literals_per_determiner
                output.append("| %d | %d-%d | %s | %s | %s | %s |"
                              % (determiner, first_row,
                                 first_row + literals_per_determiner - 1,
                                 conflict, unit, done, position))
            output.append("")

        if has_combining_tree and operation["data_lines"]:
            conflict, unit, done, literal_position, clause_position = \
                combining_tree_snapshot_of_line(operation["data_lines"][-1], column_index)
            named_row = "-"
            if unit == "1" and all(character in "01" for character in
                                   clause_position + literal_position):
                literals_per_determiner = row_count // determiner_count
                named_row = str(int(clause_position, 2) * literals_per_determiner
                                + int(literal_position, 2))
            output.append("| Combining tree | CONF | UP | DONE | CID_out | LID_out | Row named |")
            output.append("|---|---|---|---|---|---|---|")
            output.append("| chip | %s | %s | %s | %s | %s | %s |"
                          % (conflict, unit, done, clause_position, literal_position, named_row))
            output.append("")

    return "\n".join(output) + "\n"


def main(argument_values):
    if len(argument_values) < 2:
        sys.stderr.write("usage: python3 vec_to_markdown.py <checked.vec> [report.md]\n")
        return 1
    vec_path = argument_values[1]
    with open(vec_path) as vec_file:
        lines = vec_file.readlines()

    (column_index, vid_bit_count, row_count, determiner_count, address_bit_count,
     has_combining_tree) = parse_header(lines)
    operations = split_into_operations(lines)
    markdown = emit_markdown(operations, column_index, vid_bit_count, row_count,
                             determiner_count, address_bit_count, has_combining_tree)

    if len(argument_values) > 2:
        with open(argument_values[2], "w") as output_file:
            output_file.write(markdown)
        sys.stderr.write("wrote %s\n" % argument_values[2])
    else:
        sys.stdout.write(markdown)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
