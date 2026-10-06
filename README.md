# Time-Travel Debugger

I have started building the server for the Time-Travel Debugger in C++.

So far, the code checks function boundaries, saves received trace data to
`source.bin`, and creates `resolve.bin` with an offset and line size for each
instruction. It also looks up function names and patches call records with
their function offsets. It returns the offset of `main` and rejects calls to
functions that are not defined. Source instructions are read one line at a
time, and blank lines are skipped. Each resolve record stores the byte offset,
the instruction size, and the instruction text. The source file limit is
15 MB.
