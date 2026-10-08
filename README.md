# Time-Travel Debugger

## What I have done

I have started writing the server side of the Time-Travel Debugger in C++.
It receives trace data and saves it to `source.bin` in small blocks. The
receiver stops if the trace is larger than 15 MB.

The source reader goes through the file one line at a time and skips blank
lines. The validation function checks that function definitions have names,
are not nested, and have a matching `func_end`.

The `resolve` function writes each instruction to `resolve.bin` with its byte
offset and size. It also keeps track of function names, finds the `main`
function, and replaces the names in `call` instructions with the target
function offsets. If a call refers to a function that was not defined, it
returns an error.

## Instruction tokens and arithmetic

I added a tokenizer that separates a line into words. It marks the first word
as the instruction, the second as an identifier, and any remaining words as
parameters. The tokenizer also checks that the line fits in the token array.

The arithmetic helper handles `set`, `add`, `sub`, `mul`, and `div`. It reads
numbers or existing variables, updates the current frame's variables, and
rejects invalid numbers, missing variables, division by zero, and results
outside the `int32` range.

These token and arithmetic helpers are in place for the execution stage. They
are not yet connected to a complete run of the trace; call-stack execution,
snapshots, and the `.tdbg` output still need to be added.
