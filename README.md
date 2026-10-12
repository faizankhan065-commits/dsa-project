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

## Running instructions and saving history

The `executeProgram` helper starts at the offset of `main` in `resolve.bin`.
When it reaches a `call`, it finds the function header, checks that the
arguments match the function parameters, and adds a new frame to the call
stack. It passes argument values into that frame and, when a function changes
one of its arguments, copies the updated value back to the caller's variable.
When it reaches `func_end`, it returns to the saved position in the calling
function.

After each instruction, the code copies the current call stack into a
snapshot, including the function names, arguments, local variables, and return
positions in the stack. `Timeline` keeps those snapshots in order in a doubly
linked list, so later code can use them to write the execution history to the
`.tdbg` file.

The receive, resolve, and execution helpers are implemented separately so
far; the server entry point and `.tdbg` file writing still need to connect
them into the full client-server flow.
