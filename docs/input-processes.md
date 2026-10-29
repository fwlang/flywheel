# Input and child processes

`input() returns string` reads one line from standard input. `input(string prompt)` writes and flushes the prompt first, without adding a newline. `core.input` is also available after `import core`. The returned string excludes the line ending (LF or CRLF), preserves other whitespace, and can be empty. A final line without a newline is returned normally. EOF before any characters, read failures, and prompt write failures raise `InputError`; EOF does not return an empty string.

```flywheel
try
    var name = input("Name: ")
    print "Hello, {name}!"
catch InputError error
    print error.message
end
```

The `os` library supports two blocking process functions:

| Function | Execution | Return type |
|---|---|---|
| `os.shell(string)` | `/bin/sh -c` on POSIX; system `cmd.exe /d /s /c` on Windows | `int` |
| `os.command(string[])` | First array item is the executable, remaining items are individual arguments; executable search uses the platform's process search rules | `int` |

```flywheel
import os

var shell_status = os.shell("echo hello && echo goodbye")
var status = os.command(["git", "status", "--short"])
print "git exited with {status}"
```

Both wait until the child exits and inherit standard input, standard output, standard error, environment, and working directory. Flywheel flushes pending standard output/error before launch. Output goes directly to the inherited streams; the functions return the exit status rather than captured output. Status `0` conventionally means success. Nonzero child exits are returned normally. On POSIX, termination by a signal returns the negative signal number, as with `subprocess.run(...).returncode`; Windows returns the process exit code.

`command` requires a nonempty array and a nonempty executable name. Each argument is passed separately: spaces, empty strings, quotes, Unicode, and shell punctuation remain part of that argument. It does not expand variables, split strings, interpret pipes or redirections, or automatically run Windows batch files through a shell. Windows argument quoting follows the C runtime argv convention; programs with custom command-line parsers can use different conventions.

`shell` interprets its string using the platform shell, including that shell's quoting, expansion, pipelines, and redirection rules. Use `command` when passing literal arguments. Embedded NUL bytes are rejected by both APIs. Invalid arguments and launch/wait failures raise `ProcessError`. For `shell`, a missing command inside a successfully launched shell is a shell exit status, rather than a launch error.

These are native synchronous APIs, with no Python dependency. Captured output, timeout/cancellation, custom environment/cwd, pipes, background processes, and a structured `CompletedProcess` return are not implemented.
