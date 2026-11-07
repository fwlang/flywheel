# Reactive variables and negated aliases

## `reactive`, `when`, and `computed`

A `reactive` variable can be watched. `when condition do ... end` runs its body each time the condition **becomes** true, and `computed` declares a value derived from reactive variables:

```flywheel
reactive int score = 0
computed bool winning = score >= 10

when winning do
    print "winning with {score}"
end

score += 4          # nothing: winning is still false
score += 7          # prints "winning with 11"
score += 1          # nothing: winning stays true
score = 0           # winning becomes false, so the when re-arms
score = 12          # prints "winning with 12"
```

`reactive` and `computed` precede a declaration: `reactive bool alive = true`, `reactive var name = "a"`, `computed string label = name.upper()`.

A `when` behaves like this:

| Situation | Behavior |
|---|---|
| The condition is true when the `when` runs | the body runs immediately |
| The condition changes from false to true | the body runs |
| The condition stays true | the body does not run again |
| The condition becomes false | the `when` re-arms |
| The condition becomes true again | the body runs again |
| The enclosing block ends | the `when` stops watching |

Details:

- **Dependency tracking:** dependencies are found automatically from the reactive and computed variables that the condition names. The condition must name at least one. Variables read only inside functions that the condition calls are not tracked.
- **Synchronous evaluation:** checks run during the assignment that changes a dependency, before the next statement. Every change is seen; there is no batching.
  - A `when` whose body assigns its own dependencies does not re-run itself while its condition stays true.
- **Computed values update eagerly:** they are recomputed as soon as a dependency changes, so reading one is an ordinary variable read.
  - Assigning to a computed value is a compile error.
  - A computed value is itself reactive, so `when` and other `computed` declarations can depend on it.
- **Assignments that notify:** any assignment to a reactive variable notifies its watchers, including `+=` and the other compound forms, and assignments inside functions, methods, and lambdas.
  - Assignments to fields, and assignments made through another module (`other.x = 1`), are not tracked.
- **Unregistering:** a `when` or `computed` inside a function, loop, `if`, or `try` block unregisters when that block ends (like `defer`). At the top level of a file they stay active for the whole program.

```flywheel
function watch_until(int target) returns void
    reactive int x = 0
    when x == target do
        print "reached", x
    end
    for i in 1..target do
        x = i
    end
end                    # the when stops watching here
```

## Negated aliases

`bool name/negated = value` declares a bool and a second name for its negation. Both names refer to the same value:

```flywheel
bool enabled/disabled = true
print enabled, disabled     # true false
disabled = true             # stores enabled = false
print enabled, disabled     # false true
```

- **Reading:** reading `disabled` gives `not enabled`.
- **Assigning:** assigning `disabled = x` stores `enabled = not x`.
- **Scope:** both names share the declaration's scope and lifetime.
- **Types:** negated aliases are only for `bool`.
- **Passing to functions:** passing an alias to a function passes its current value.
- **Restrictions:**
  - Compound assignment (`disabled += ...`) is rejected.
  - Aliases cannot be fields or parameters.
  - A declaration has exactly two names.

Aliases combine with `reactive`. A `when` on the alias watches the underlying variable:

```flywheel
reactive bool playerAlive/playerDead = true

when playerDead do
    print "Game over!"
end

playerAlive = false    # prints "Game over!"
```

## Implementation

The compiler lowers these features before type checking (see `src/lower.cpp`):

- **`reactive` variables:** each one gets a hidden list of watcher closures. An assignment to the variable then calls each watcher.
- **`when`:** becomes a closure holding the condition and an armed flag. It is added to the watcher list of each dependency and called once when the `when` statement runs.
- **`computed` values:** a computed value is a reactive variable plus a watcher that reassigns it.
- **Name resolution:** names are resolved by lexical scope, so a local or parameter that shadows a reactive variable is an ordinary variable.
