# Time-Travel Debugger

I have added the function lookup and call patching needed for `resolve.bin`.
The code saves each function's offset, then uses it to update calls and find
`main`. I also added a small `Token` structure with a type and text. It will
help label instruction parts when execution is added.
