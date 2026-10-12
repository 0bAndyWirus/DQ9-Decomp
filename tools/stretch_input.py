#!/usr/bin/env python3
"""Stretch events and merge overlaps.

Overlapping button masks are ORed. On native input, the most recent active touch
event with coordinates wins; an overlapping button-only event does not cancel an
earlier touch. Oracle actions use the same precedence: button sets are unioned and
the last action carrying `touch` wins at each output frame.
"""
from __future__ import annotations

import argparse
from bisect import bisect_right
import json
from pathlib import Path


def _check_duration(duration: int) -> None:
    if isinstance(duration, bool) or not isinstance(duration, int) or duration < 1:
        raise ValueError("duration must be a positive integer")


def stretch_native(text: str, duration: int = 12) -> str:
    _check_duration(duration)
    frames: dict[int, tuple[int, int, int]] = {}
    for line_number, line in enumerate(text.splitlines(), 1):
        fields = line.split()
        if not fields:
            continue
        if len(fields) != 4:
            raise ValueError(f"line {line_number}: expected exactly 4 fields (frame mask x y)")
        try:
            frame, mask, x, y = map(int, fields)
        except ValueError as exc:
            raise ValueError(f"line {line_number}: expected integer frame, mask, x and y") from exc
        if frame < 0:
            raise ValueError(f"line {line_number}: frame must be non-negative")
        if mask < 0:
            raise ValueError(f"line {line_number}: button mask must be non-negative")
        if (x, y) != (-1, -1) and not (0 <= x < 256 and 0 <= y < 192):
            raise ValueError(f"line {line_number}: touch coordinates must be -1 -1 or within 0..255 x 0..191")
        for active_frame in range(frame, frame + duration):
            old_mask, old_x, old_y = frames.get(active_frame, (0, -1, -1))
            frames[active_frame] = (old_mask | mask, x, y) if x >= 0 else (old_mask | mask, old_x, old_y)
    return "".join(f"{frame} {mask} {x} {y}\n" for frame, (mask, x, y) in sorted(frames.items()))


def _validate_action(action: object, index: int, line_number: int) -> tuple[int, list[str], tuple[int, int] | None]:
    label = f"line {line_number} action {index}"
    if not isinstance(action, dict):
        raise ValueError(f"{label}: must be an object")
    if "frame" not in action:
        raise ValueError(f"{label}: missing frame")
    frame = action["frame"]
    if isinstance(frame, bool) or not isinstance(frame, int) or frame < 0:
        raise ValueError(f"{label}: frame must be a non-negative integer")
    unknown = set(action) - {"frame", "buttons", "touch"}
    if unknown:
        raise ValueError(f"{label}: unknown field(s): {', '.join(sorted(map(str, unknown)))}")
    buttons = action.get("buttons", [])
    if not isinstance(buttons, list) or any(not isinstance(button, str) or not button.strip() for button in buttons):
        raise ValueError(f"{label}: buttons must be an array of non-empty strings")
    supported_buttons = {"A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L", "X", "Y"}
    if any(button.upper() not in supported_buttons for button in buttons):
        raise ValueError(f"{label}: unknown button name")
    touch = None
    if "touch" in action:
        touch_value = action["touch"]
        if (not isinstance(touch_value, list) or len(touch_value) != 2 or
                any(isinstance(value, bool) or not isinstance(value, int) for value in touch_value)):
            raise ValueError(f"{label}: touch must be a two-integer array")
        x, y = touch_value
        if not (0 <= x < 256 and 0 <= y < 192):
            raise ValueError(f"{label}: touch coordinates out of range: {touch_value}")
        touch = (x, y)
    return frame, buttons, touch

def _action_line_numbers(text: str) -> list[int]:
    decoder = json.JSONDecoder()
    line_starts = [0]
    line_starts.extend(index + 1 for index, char in enumerate(text) if char == "\n")
    index = 0
    while index < len(text) and text[index].isspace():
        index += 1
    index += 1  # opening '['; json.loads has already validated the container
    lines = []
    while index < len(text):
        while index < len(text) and (text[index].isspace() or text[index] == ","):
            index += 1
        if index >= len(text) or text[index] == "]":
            break
        start = index
        _, index = decoder.raw_decode(text, index)
        lines.append(bisect_right(line_starts, start))
    return lines


def stretch_oracle(text: str, duration: int = 12) -> str:
    _check_duration(duration)
    try:
        actions = json.loads(text)
    except json.JSONDecodeError as exc:
        raise ValueError(f"line {exc.lineno}: invalid JSON: {exc.msg}") from exc
    if not isinstance(actions, list):
        raise ValueError("line 1: expected a JSON array of actions")
    frames: dict[int, dict[str, object]] = {}
    action_lines = _action_line_numbers(text)
    for index, (action, line_number) in enumerate(zip(actions, action_lines), 1):
        frame, buttons, touch = _validate_action(action, index, line_number)
        for active_frame in range(frame, frame + duration):
            state = frames.setdefault(active_frame, {"buttons": set()})
            state["buttons"].update(buttons)
            if touch is not None:
                state["touch"] = list(touch)
    result = []
    for frame, state in sorted(frames.items()):
        item: dict[str, object] = {"frame": frame}
        if state["buttons"]:
            item["buttons"] = sorted(state["buttons"])
        if "touch" in state:
            item["touch"] = state["touch"]
        result.append(item)
    return json.dumps(result, indent=2) + "\n"


def stretch(text: str, format: str, duration: int = 12) -> str:
    _check_duration(duration)
    if format == "native":
        return stretch_native(text, duration)
    if format == "oracle":
        return stretch_oracle(text, duration)
    raise ValueError(f"unknown input format: {format}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("format", choices=("native", "oracle"))
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--frames", type=int, default=12, help="held frames per input event (default: 12)")
    args = parser.parse_args(argv)
    try:
        result = stretch(args.source.read_text(), args.format, args.frames)
        args.destination.parent.mkdir(parents=True, exist_ok=True)
        args.destination.write_text(result)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
