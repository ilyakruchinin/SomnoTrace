"""Shared QEMU display profiles and strict shell-launcher options."""
import argparse

BOARDS = ("7b", "154")


def target(board="7b"):
    if board not in BOARDS:
        raise ValueError(f"unsupported QEMU board: {board}")
    compact = board == "154"
    suffix = "-154" if compact else ""
    return {
        "board": board,
        "width": 240 if compact else 1024,
        "height": 240 if compact else 600,
        "build_dir": f"build-qemu{suffix}",
        "sdkconfig": f"sdkconfig.qemu{suffix}",
        "defaults": "sdkconfig.defaults;sdkconfig.qemu.defaults" +
                    (";sdkconfig.qemu-154.defaults" if compact else ""),
        "ready_log": "240x240 original-board UI preview ready" if compact else
                     "1024x600 interactive UI preview ready",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("build", "run", "test"))
    parser.add_argument("--board", choices=BOARDS, default="7b")
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--clean", action="store_true")
    args = parser.parse_args()
    if args.build and args.mode == "build":
        parser.error("--build is only supported by run-qemu-ui.sh and test-qemu-ui.sh")
    if args.clean and args.mode != "build":
        parser.error("--clean is only supported by build-qemu.sh")
    profile = target(args.board)
    # Tab-delimited data consumed with read, never evaluated as shell code.
    print("\t".join(str(profile[key]) for key in
                    ("board", "build_dir", "sdkconfig", "defaults")) +
          f"\t{int(args.build)}\t{int(args.clean)}")


if __name__ == "__main__":
    main()
