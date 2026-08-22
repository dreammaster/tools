"""
Generic companion script for rtlink_decode's <exe>.polymorphic.txt report --
works against the IDA database for any RTLink-decoded executable, not just
one specific game.

rtlink_decode writes that report when a far call/jmp (or other segment-valued
word, e.g. "mov reg, seg X") targets a memory slot with multiple mutually-
exclusive alternate segments and can't be resolved to a single one -- only
one alternate is ever resident at a time, picked by game state at runtime,
so no static relocation can point at a single correct target. rtlink_decode
leaves the raw word untouched rather than guess wrong; this script just makes
those spots visible in the idb by dropping a comment at each one, one raw
file offset per line (hex, trailing 'h') from the report.

Expects the report at <input_file>.polymorphic.txt next to the .idb's own
input file -- that's where rtlink_decode writes it, alongside the .exe it
just decoded. Override REPORT_PATH below if it lives elsewhere.

Convention: DRY_RUN starts True. Run once with DRY_RUN True, check the
output, then flip and re-run. Safe to re-run any time (e.g. after decoding
a newer build) -- already-commented locations are skipped.

    .\\run_ida_script.ps1 -Idb <decoded_exe_idb> -ScriptName apply_polymorphic_comments.py -NoExport
"""

import os

import idc
import ida_loader
import ida_bytes

DRY_RUN = True

REPORT_PATH = None  # override to point at a specific .polymorphic.txt; None = auto-derive

COMMENT_PREFIX = "rtlink_decode: polymorphic slot"


def report_path():
    if REPORT_PATH:
        return REPORT_PATH
    input_path = idc.get_input_file_path()
    return input_path + ".polymorphic.txt"


def read_offsets(path):
    offsets = []
    with open(path, "r") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            offsets.append(int(line.rstrip("hH"), 16))
    return offsets


def describe(word_ea):
    """Classify what kind of instruction this segment word belongs to, so
    the comment can say something more specific than just 'unresolved'."""
    opcode_ea = word_ea - 3
    opcode = ida_bytes.get_byte(opcode_ea)
    if opcode == 0x9A:
        return "far call operand", opcode_ea
    if opcode == 0xEA:
        return "far jmp operand", opcode_ea
    # Not a far call/jmp -- fall back to whatever instruction IDA thinks
    # starts at or before the word (e.g. "mov reg, seg X").
    head = idc.get_item_head(word_ea)
    return "segment value", head


def main():
    path = report_path()
    print(f"DRY_RUN = {DRY_RUN}")
    print(f"report  = {path}")
    if not os.path.exists(path):
        print("report file not found -- nothing to do")
        return

    offsets = read_offsets(path)
    print(f"{len(offsets)} flagged file offset(s) in report")

    applied = 0
    already = 0
    unmapped = 0
    stuck = 0
    for file_offset in offsets:
        word_ea = ida_loader.get_fileregion_ea(file_offset)
        if word_ea == idc.BADADDR:
            print(f"  {file_offset:#x}: no EA mapping found, skipping")
            unmapped += 1
            continue

        kind, comment_ea = describe(word_ea)
        text = f"{COMMENT_PREFIX} ({kind}, target depends on which overlay is loaded)"

        # set_cmt() can return True while silently doing nothing if comment_ea
        # is a "tail" byte IDA hasn't attributed to a proper item head (seen
        # when the far call/jmp opcode 3 bytes back isn't itself recognized as
        # an instruction start) -- so a comment for this site may have landed
        # on a fallback anchor rather than comment_ea itself. Check every
        # candidate anchor for an existing comment before assuming it's unset.
        candidates = [comment_ea, idc.get_item_head(word_ea), word_ea]

        if any(idc.get_cmt(c, 0) == text for c in candidates):
            already += 1
            continue

        if DRY_RUN:
            print(f"  {file_offset:#x} -> ea {comment_ea:#x}: {text!r}")
            applied += 1
            continue

        landed_ea = None
        for candidate_ea in candidates:
            idc.set_cmt(candidate_ea, text, 0)
            if idc.get_cmt(candidate_ea, 0) == text:
                landed_ea = candidate_ea
                break

        if landed_ea is None:
            print(f"  {file_offset:#x} -> ea {comment_ea:#x}: comment did not persist at any candidate anchor "
                  f"({', '.join(f'{c:#x}' for c in candidates)})")
            stuck += 1
        else:
            if landed_ea != comment_ea:
                print(f"  {file_offset:#x}: comment_ea {comment_ea:#x} wasn't a valid anchor, used {landed_ea:#x} instead")
            applied += 1

    print(f"\n{applied} comment(s) {'would be ' if DRY_RUN else ''}applied, "
          f"{already} already commented, {unmapped} unmapped, {stuck} stuck (see above)")


main()
