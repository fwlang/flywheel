# Input, output, and child processes

## Terminal input and output

`input() returns string` reads one line from standard input. `input(string prompt)` writes and flushes the prompt first, without adding a newline. The returned string excludes the line ending (LF or CRLF). EOF before any characters, read failures, and prompt write failures raise `InputError`.

The `io` module adds stream-style access:

| Function | Behavior |
|---|---|
| `io.read_line()` | `string?`: the next line, or null at end of input |
| `io.read_all()` | the rest of standard input |
| `io.write(value)`, `io.write_error(value)` | write to stdout / stderr without a newline |
| `io.flush()` | flush stdout and stderr |
| `io.open(path, mode = "r")` | open an `io.file`; modes `"r"`, `"w"`, `"a"`, `"r+"` |

An `io.file` has `read_line()` (`string?`), `read_all()`, `lines()`, `write(value)`, and `close()`. Files close automatically when their last reference is released. Failures raise `FileError`.

```flywheel
import io
var log = io.open("log.txt", "a")
log.write("started\n")
log.close()
```

## Program arguments and exit

`os.args()` returns the program's arguments (excluding the program name). `flywheel run app.fw -- one two` passes `one` and `two`. `os.exit(code = 0)` flushes output and ends the program immediately; `finally` blocks do not run.

## Running processes with captured output

`os.run(string[] command)` runs a program, waits for it, and returns an `os.result`:

```flywheel
import os
var r = os.run(["git", "status", "--short"])
print r.status, r.stdout, r.stderr, r.timed_out
```

For more control, build an `os.process`, adjust its fields, and call `run()`:

```flywheel
import os
var p = os.process(["sort"])
p.input = "b\na\n"          # written to the child's stdin
p.cwd = "/tmp"              # working directory ("" keeps the current one)
p.env["LANG"] = "C"         # added to (or replacing) inherited variables
p.inherit_env = true        # false starts from an empty environment plus `env`
p.timeout = 5.0             # seconds; 0 waits forever; on expiry the child is killed
p.capture = true            # false lets the child use this program's stdout/stderr
var result = p.run()
```

`os.result` has `status` (the exit code; on POSIX a signal termination gives the negative signal number), `stdout`, `stderr`, and `timed_out`. Output is read while input is written, so large inputs and outputs do not deadlock.

`os.pipeline(commands, input = "")` connects processes stdout-to-stdin, like a shell pipeline, and returns the last stage's status and stdout; stderr from every stage is collected together:

```flywheel
import os
var sorted = os.pipeline([["printf", "b\\na\\n"], ["sort"]])
print sorted.stdout.lines()
```

On POSIX all stages run at once. On Windows stages run one after another, each fed the previous stage's output (same result for finite data, not streaming); this path is untested.

## Inherited-stream helpers

| Function | Execution | Return type |
|---|---|---|
| `os.shell(string)` | `/bin/sh -c` on POSIX; system `cmd.exe /d /s /c` on Windows | `int` |
| `os.command(string[])` | First item is the executable, remaining items are individual arguments | `int` |

Both wait for the child and inherit standard input, output, error, environment, and working directory, returning the exit status. Flywheel flushes pending output before launching any process.

Every argument is passed separately: spaces, empty strings, quotes, Unicode, and shell punctuation remain part of that argument, and nothing is expanded or split. Only `os.shell` interprets shell syntax. Embedded NUL bytes are rejected. Invalid arguments and launch/wait failures raise `ProcessError`; a nonzero exit is returned normally.
