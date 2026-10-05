# Time-Travel Debugger

This is a small C++ project for working with program traces.

The server code checks that function definitions are properly closed and
receives trace data through an input stream. It saves the data to `source.bin`
in small blocks and rejects files larger than 15 MB.
