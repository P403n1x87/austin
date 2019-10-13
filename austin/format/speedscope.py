import json

from austin.stats import parse_line


def _generate_profiles(source: any):
    shared_frames = []
    frame_index = {}

    profiles = {}

    def get_profile(name, unit):
        if name not in profiles:
            profiles[name] = {
                "type": "sampled",
                "name": name,
                "unit": unit,
                "startValue": 0,
                "endValue": 0,
                "samples": [],
                "weights": [],
            }

        return profiles[name]

    def add_frames_to_thread_profile(thread_profile, frames, metric):
        stack = []
        for frame in frames:
            frame_id = f"{frame[0]}@{frame[1]}"
            if frame_id not in frame_index:
                frame_index[frame_id] = len(shared_frames)

                frame_name, frame_file = frame[0].split(maxsplit=1)
                frame_file = frame_file[1:-1]
                frame_line = int(frame[1][1:])
                shared_frames.append(
                    {"name": frame_name, "file": frame_file, "line": frame_line}
                )

            stack.append(frame_index[frame_id])

        thread_profile["samples"].append(stack)
        thread_profile["weights"].append(metric)
        thread_profile["endValue"] += metric

    # Assume full metrics
    line = next(source)
    full = True
    try:
        parse_line(line, True)
    except ValueError as e:
        full = False

    for line in source:
        if b"Bad sample" in line:
            continue

        process, thread, frames, metrics = parse_line(line, full)

        frames = [(frames[2 * i], frames[2 * i + 1]) for i in range(len(frames) >> 1)]
        if process:
            thread = f"Thread {process.split()[1]}:{thread.split()[1]}"

        add_frames_to_thread_profile(
            get_profile(f"Time profile of {thread}", "microseconds"), frames, metrics[0]
        )

        if full:
            add_frames_to_thread_profile(
                get_profile(f"Memory allocation profile of {thread}", "bytes"),
                frames,
                metrics[1] << 10,
            )

            add_frames_to_thread_profile(
                get_profile(f"Memory release profile of {thread}", "bytes"),
                frames,
                (-metrics[2]) << 10,
            )

    return shared_frames, profiles


def _generate_json(frames, profiles, name):
    return {
        "$schema": "https://www.speedscope.app/file-format-schema.json",
        "shared": {"frames": frames},
        "profiles": sorted(
            [profile for _, profile in profiles.items()],
            key=lambda profile: profile["name"].rsplit(maxsplit=1)[-1],
        ),
        "name": name,
        "exporter": "Austin2Speedscope Converter 0.1.0",
    }


def to_speedscope(source: any, name: str):
    """Convert a list of collapsed samples to the speedscope JSON format.

    The result is a Python ``dict`` that complies with the Speedscope JSON
    schema and that can be exported to a JSON file with a straight call to
    ``json.dump``.

    Args:
        source (any): Any object that behaves like a generator of strings, e.g.
            an open file.

        name ()

        full (bool): Whether to treat each line as having a full set of metrics.

    Returns:
        (dict): a dictionary that complies with the speedscope JSON schema.
    """
    return _generate_json(*_generate_profiles(source), name)


def main():
    import os, sys
    from argparse import ArgumentParser

    arg_parser = ArgumentParser(
        prog="austin2speedscope",
        description=(
            "Convert Austin generated profiles to the Speedscope JSON format "
            "accepted by https://speedscope.app. The output will contain a profile "
            "for each thread and metric included in the input file."
        ),
    )

    arg_parser.add_argument(
        "input",
        type=str,
        help="The input file containing Austin samples in normal format.",
    )
    arg_parser.add_argument(
        "output", type=str, help="The name of the output Speedscope JSON file."
    )
    arg_parser.add_argument(
        "--indent", type=int, help="Give a non-null value to prettify the JSON output."
    )

    arg_parser.add_argument("-V", "--version", action="version", version="0.1.0")

    args = arg_parser.parse_args()

    try:
        with open(args.input, "rb") as fin:
            json.dump(
                to_speedscope(fin, os.path.basename(args.input)),
                open(args.output, "w"),
                indent=args.indent,
            )
    except FileNotFoundError:
        print(f"No such input file: {args.input}")
        exit(1)


if __name__ == "__main__":
    main()
