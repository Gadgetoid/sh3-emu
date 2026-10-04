#!/usr/bin/env python3
import argparse
import os
import random
import subprocess
import sys
import tempfile

LINKER_SCRIPT = """
ENTRY(_start)
SECTIONS
{
    . = 0x400000;
    .text : { *(.text) }
    . = 0x480000;
    .data : { *(.data) }
}
"""

ARITHMETIC = [
    "add", "addc", "addv", "sub", "subc", "subv", "and", "or", "xor", "not", "neg", "negc",
    "tst", "cmp/eq", "cmp/hs", "cmp/ge", "cmp/hi", "cmp/gt", "cmp/str", "mov",
    "extu.b", "extu.w", "exts.b", "exts.w", "swap.b", "swap.w", "xtrct",
    "mul.l", "muls.w", "mulu.w", "dmuls.l", "dmulu.l", "shad", "shld", "div1",
]
SINGLE = [
    "shll", "shlr", "shal", "shar", "rotcl", "rotcr", "shll2", "shlr2",
    "shll8", "shlr8", "shll16", "shlr16", "dt", "cmp/pz", "cmp/pl", "movt",
]
FLAGS = ["sett", "clrt", "sets", "clrs", "div0u", "clrmac"]
DATA_REGISTERS = list(range(0, 12))


def register(rng):
    return f"r{rng.choice(DATA_REGISTERS)}"


def slot_instruction(rng):
    choice = rng.random()
    if choice < 0.6:
        operation, source, target = rng.choice(ARITHMETIC), register(rng), register(rng)
        if operation == "div1":
            operation = "sub"
        return f"{operation} {source},{target}"
    if choice < 0.9:
        return f"{rng.choice(SINGLE)} {register(rng)}"
    return f"add #{rng.randint(-128, 127)},{register(rng)}"


def memory_instruction(rng):
    size = rng.choice("bwl")
    scale = {"b": 1, "w": 2, "l": 4}[size]
    kind = rng.randint(0, 6)
    target = register(rng)
    if kind == 0:
        return [f"mov.{size} @r14,{target}"] if rng.random() < 0.5 else [f"mov.{size} {target},@r14"]
    if kind == 1:
        offset = rng.randint(0, 63 // scale * scale) // scale * scale
        return [f"mov r14,r12", f"add #{offset},r12", f"mov.{size} @r12+,{target}"]
    if kind == 2:
        offset = rng.randint(1, 31) * scale
        return [f"mov r14,r12", f"add #{offset},r12", f"mov.{size} {target},@-r12"]
    if kind == 3:
        offset = rng.randint(0, 15) * scale
        if size == "l":
            if rng.random() < 0.5:
                return [f"mov.l @({offset},r14),{target}"]
            return [f"mov.l {target},@({offset},r14)"]
        if rng.random() < 0.5:
            return [f"mov.{size} @({offset},r14),r0"]
        return [f"mov.{size} r0,@({offset},r14)"]
    if kind == 4:
        offset = rng.randint(0, 15) * scale
        return [f"mov #{offset},r0", f"mov.{size} @(r0,r14),{target}" if rng.random() < 0.5 else f"mov.{size} {target},@(r0,r14)"]
    if kind == 5:
        offset = rng.randint(0, 63)
        operation = rng.choice(["tst.b", "and.b", "or.b", "xor.b"])
        return [f"mov #{offset},r0", f"{operation} #{rng.randint(0, 255)},@(r0,gbr)"]
    offset = rng.randint(0, 63)
    return [f"mov r14,r12", f"add #{offset},r12", "tas.b @r12"]


def body_instruction(rng, label_counter):
    choice = rng.random()
    if choice < 0.40:
        operation, source, target = rng.choice(ARITHMETIC), register(rng), register(rng)
        if operation == "div1":
            if source == target:
                source = f"r{(int(target[1:]) + 1) % 12}"
            return [f"mov #{rng.choice([-1, 1]) * rng.randint(1, 127)},{source}", f"{operation} {source},{target}"]
        return [f"{operation} {source},{target}"]
    if choice < 0.52:
        return [f"{rng.choice(SINGLE)} {register(rng)}"]
    if choice < 0.56:
        return [rng.choice(FLAGS)]
    if choice < 0.60:
        return [f"div0s {register(rng)},{register(rng)}"]
    if choice < 0.64:
        operation = rng.choice(["and", "or", "xor", "tst"])
        return [f"{operation} #{rng.randint(0, 255)},r0"]
    if choice < 0.67:
        return [f"cmp/eq #{rng.randint(-128, 127)},r0"]
    if choice < 0.71:
        return [f"mov #{rng.randint(-128, 127)},{register(rng)}"] if rng.random() < 0.5 else [f"add #{rng.randint(-128, 127)},{register(rng)}"]
    if choice < 0.73:
        return [rng.choice([f"sts mach,{register(rng)}", f"sts macl,{register(rng)}", f"lds {register(rng)},mach", f"lds {register(rng)},macl"])]
    if choice < 0.76:
        size = rng.choice(["w", "l"])
        scale = 2 if size == "w" else 4
        first, second = rng.randint(0, 6) * scale, rng.randint(0, 6) * scale
        return [f"mov r14,r12", f"add #{first},r12", f"mov r14,r13", f"add #{second},r13", f"mac.{size} @r12+,@r13+"]
    if choice < 0.88:
        return memory_instruction(rng)
    label = f"skip{label_counter[0]}"
    label_counter[0] += 1
    kind = rng.choice(["bt", "bf", "bt/s", "bf/s", "bra", "bsr"])
    lines = []
    if kind in ("bt/s", "bf/s", "bra"):
        lines += [f"{kind} {label}", slot_instruction(rng)]
    elif kind == "bsr":
        lines += [f"bsr {label}", "nop", f"bra {label}_done", "nop", f"{label}:", "rts", slot_instruction(rng), f"{label}_done:"]
        return lines
    else:
        lines += [f"{kind} {label}"]
    lines += [slot_instruction(rng), f"{label}:"]
    return lines


def generate(seed, length):
    rng = random.Random(seed)
    init_values = [rng.getrandbits(32) for _ in range(12)]
    buffer_values = [rng.getrandbits(32) for _ in range(16)]
    lines = [
        "\t.text", "\t.global _start", "_start:",
        "mov.l init_pointer,r13", "mov.l buffer_pointer,r14", "ldc r14,gbr",
    ]
    lines += [f"mov.l @r13+,r{n}" for n in range(12)]
    lines += ["lds r0,mach", "lds r1,macl", "bra body", "nop", ".align 2",
              "init_pointer: .long init_values", "buffer_pointer: .long buffer", "body:"]
    counter = [0]
    for _ in range(length):
        lines += body_instruction(rng, counter)
    lines += [
        "mov.l result_pointer,r12",
        *[f"mov.l r{n},@({n * 4},r12)" for n in range(12)],
        "movt r0", "mov.l r0,@(48,r12)", "sts mach,r0", "mov.l r0,@(52,r12)",
        "sts macl,r0", "mov.l r0,@(56,r12)",
        "mov #4,r3", "mov #1,r4", "mov.l result_pointer,r5", "mov.l result_size,r6", "trapa #0x13",
        "mov #1,r3", "mov #0,r4", "trapa #0x13",
        ".align 2", "result_pointer: .long result", "result_size: .long 128",
        "\t.data", ".align 4", "init_values:",
    ]
    lines += [f".long 0x{value:08x}" for value in init_values]
    lines += [".align 4", "result:", ".space 64", "buffer:"]
    lines += [f".long 0x{value:08x}" for value in buffer_values]
    lines += [".space 64"]
    return "\n".join(f"\t{line}" if not line.endswith(":") and not line.startswith("\t") else line for line in lines) + "\n"


def build(source, directory, name, prefix):
    assembly = os.path.join(directory, name + ".s")
    obj = os.path.join(directory, name + ".o")
    elf = os.path.join(directory, name + ".elf")
    with open(assembly, "w") as handle:
        handle.write(source)
    subprocess.run([prefix + "as", "--little", "--isa=sh3", "-o", obj, assembly], check=True)
    script = os.path.join(directory, "link.ld")
    with open(script, "w") as handle:
        handle.write(LINKER_SCRIPT)
    subprocess.run([prefix + "ld", "-EL", "-z", "max-page-size=4096", "-T", script, "-o", elf, obj], check=True)
    return elf


def run_both(args, elf):
    ours = subprocess.run([args.runner, elf], capture_output=True)
    if ":" in args.reference:
        host, command = args.reference.split(":", 1)
        subprocess.run(["scp", "-q", elf, f"{host}:/tmp/sh3fuzz.elf"], check=True)
        theirs = subprocess.run(["ssh", host, f"{command} /tmp/sh3fuzz.elf"], capture_output=True)
    else:
        theirs = subprocess.run([args.reference, elf], capture_output=True)
    return ours, theirs


def shrink(args):
    with tempfile.TemporaryDirectory() as directory:
        low, high = 0, args.length
        while low + 1 < high:
            middle = (low + high) // 2
            ours, theirs = run_both(args, build(generate(args.seed, middle), directory, "shrink", args.prefix))
            if ours.stdout != theirs.stdout:
                high = middle
            else:
                low = middle
        print(generate(args.seed, high))
        print(f"first failing length {high}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--count", type=int, default=50)
    parser.add_argument("--length", type=int, default=200)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--prefix", default=os.environ.get("SH_PREFIX", "sh-elf-"))
    parser.add_argument("--shrink", action="store_true", help="find the shortest failing length and print its source")
    parser.add_argument("--runner", default="./sh3-run")
    parser.add_argument("--reference", default=os.environ.get("SH_REFERENCE", "qemu-sh4"),
                        help="command that runs an ELF; HOST:qemu-sh4 copies it to HOST over ssh")
    args = parser.parse_args()
    if args.shrink:
        shrink(args)
        return
    failures = 0
    with tempfile.TemporaryDirectory() as directory:
        for index in range(args.count):
            seed = args.seed + index
            elf = build(generate(seed, args.length), directory, f"fuzz{seed}", args.prefix)
            ours, theirs = run_both(args, elf)
            if ours.stdout != theirs.stdout or ours.returncode != theirs.returncode:
                failures += 1
                print(f"FAIL seed {seed}")
                words_ours = [int.from_bytes(ours.stdout[i:i + 4], "little") for i in range(0, len(ours.stdout), 4)]
                words_theirs = [int.from_bytes(theirs.stdout[i:i + 4], "little") for i in range(0, len(theirs.stdout), 4)]
                for i, (a, b) in enumerate(zip(words_ours, words_theirs)):
                    if a != b:
                        print(f"  word {i}: ours {a:08x} reference {b:08x}")
                if ours.stderr:
                    print(ours.stderr.decode(errors="replace"))
            else:
                print(f"ok   seed {seed}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
