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

Shared limits, snapshot structures, client metadata, protocol status values,
and CRC-32 helpers are declared in `helper.h` for use across the project.

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

## Saving the debugger file

The `writeTdbg` function writes the `TTDB` header, all snapshots, and an index
at the end of the file. The header records the format version, snapshot count,
and where the index starts. Each index entry points to its snapshot, so a
reader can jump straight to a selected step.

Snapshots are written field by field rather than copying C++ `string` objects
from memory. Each string is stored with its length, followed by its text;
integer fields use a fixed little-endian format. Each frame stores its
function name, arguments, return position, and local variables.

## Build and run the network server

The server uses POSIX TCP sockets. Build it in a Linux/POSIX-compatible environment:

```bash
g++ -std=c++11 server.cpp -o server
```

Start the server with its listening port:

```bash
./server 5001
```

The server handles one client request, reads the trace until the client closes
its write side, saves it as `source.bin`, and runs the validation, resolution,
execution, and serialization pipeline. It replies with `[status][payload][CRC-32]`
and then exits; status `0` carries the `.tdbg` file and status `1` carries an
error message. The load balancer validates the uploaded trace plus its CRC-32
trailer, then validates the complete response before forwarding it. The client
stages downloads separately and replaces the previous output only after the
response checksum is valid.

## Run the client

Build the load balancer and client in the same POSIX-compatible environment:

```bash
g++ -std=c++11 -pthread load_balancer.cpp -o load_balancer
g++ -std=c++11 client.cpp -o client
```

Start the load balancer in one terminal, then start the client in another:

```bash
./load_balancer
./client 127.0.0.1 5000
```

The client supports `start <source-file>`, `next`, `prv`, `jmp next <n>`,
`jmp prv <n>`, `help`, and `quit` (or `exit`). Quote a source path containing
spaces. Navigation stays local to the validated `.tdbg` session; a failed
download, invalid session file, or failed navigation does not replace or
advance the current session.
