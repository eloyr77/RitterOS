# js: run JavaScript (a prompt without a file)

```
js [FILE [ARG...]] | js -e CODE [ARG...]
js FILE [ARG...]
js -e CODE [ARG...]
js
```

Runs JavaScript with RitterOS's own engine (`user/lib/js/`), the one
iBrowser runs its pages' scripts with: a script from FILE, the text after
`-e`, or, with neither, a prompt `js> ` that runs each line typed (or
read from the standard input, up to 4096 bytes a line) and prints its
value unless it is `undefined`; Ctrl-D ends it. The words after FILE or
CODE are the global array `arguments` (`arguments[0]` the first).
`print(...)` and `console.log(...)` (also `console.info`, `console.debug`)
write their arguments, separated by spaces, as one line on the standard
output; `console.error` and `console.warn` on the standard error. After
the script `js` runs the jobs it queued (the Promises' reactions) until
none is left, as JavaScriptCore's shell does. `js` came in step 27o;
generators, Promises and `async` in 27r, `Intl` in 27s, `Proxy` in 64r.

**The language** is ECMAScript 2023 except async generators (and `for
await`), modules, `with`, BigInt and typed arrays: `let` and
`const`, arrow functions, classes with fields, private names, static
blocks and `super`, destructuring, spread and rest, template literals,
optional chaining, `??` and the logical assignments, labeled loops,
getters and setters, Symbols and iterators, generators (`function*`,
`yield`, `yield*`), Promises and `async` functions with `await`. The
standard objects: Object, Function, Array, String, Number, Boolean,
Symbol, Math, JSON, RegExp (lookbehind, named groups, the flags
`dgimsuy`), Map, Set, WeakMap, WeakSet, Date, Error and its kinds,
Reflect, Proxy (every trap, with the standard's checks of what a trap
answers, and `Proxy.revocable`), Promise, and `Intl` in English (`en-US`: NumberFormat,
DateTimeFormat, PluralRules, Collator, RelativeTimeFormat, ListFormat,
Locale; about fifty named time zones). `eval` runs its text in the global
scope. Case mapping follows the Unicode Character Database 17.0 for every
script (`"ß".toUpperCase()` is `SS`). The manual's section on `js` has
the details.

`js` takes no other options; a FILE starting with `-` is refused.

## Exit status and messages

- `0`: the script ran to its end (at the prompt always 0).
- `1`: `js: cannot read FILE`, or `js: out of memory` at the start.
- `2`: an unknown option, or `-e` without CODE: `usage: js [FILE
  [ARG...]] | js -e CODE`.
- `3`: an uncaught exception, or a syntax error: `js: uncaught NAME:
  MESSAGE at FILE:LINE` on the standard error (`-e` is the file's name for
  the text of `-e`, `(prompt)` at the prompt; a thrown value that is not
  an Error is shown as its text). An exception in a queued job prints the same and
  ends with 3 too. At the prompt the message comes and the prompt goes
  on.

## Examples

```
ritteros:/$ js -e 'print([3, 1, 2].sort(), 2 ** 10)'
1,2,3 1024
ritteros:/$ js -e 'print(arguments.length, arguments[1])' a b
2 b
ritteros:/$ js count.js 10
ritteros:/$ js -e 'throw new Error("stop")'
js: uncaught Error: stop at -e:1
ritteros:/$ js
js> var a = [1, 2, 3]
js> a.map(x => x * x)
1,4,9
js> new Intl.NumberFormat("en-US").format(1234567.891)
1,234,567.891
js>
```

## See also

[`ibrowser`](ibrowser.md), [`awk`](awk.md), [`sh`](sh.md)
