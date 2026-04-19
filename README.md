# c4deb

An interactive source-level debugger built on top of [c4](https://github.com/rswier/c4) — the self-hosting C interpreter in ~500 lines.
c4deb adds breakpoints, watches, register/stack inspection, and memory inspection to c4's VM, all driven from a REPL prompt.

## Build

```sh
cc -o c4deb c4deb.c
```

## Usage

```
./c4deb [-s] [-d] [-t] <source.c> [args...]
```

| Flag | Description |
|------|-------------|
| `-s` | Compile and print each source line alongside its emitted bytecode; do not execute |
| `-d` | Non-interactive trace: print every VM instruction as it executes |
| `-t` | Interactive debugger: pause at each source line and accept commands |

```sh
./c4deb hello.c               # run normally
./c4deb -t hello.c            # interactive debug session
./c4deb c4.c hello.c          # run c4.c inside c4deb (two-level execution)
./c4deb -t c4.c hello.c       # debug the c4 compiler as it compiles hello.c
```

## Debugger Commands

When running with `-t`, execution pauses at each new source line and shows a `(deb) >` prompt.

### Execution control

| Command | Description |
|---------|-------------|
| `s` or Enter | Step — execute one VM instruction (steps into function calls) |
| `n` | Step over — run until the current call depth returns |
| `n <func>` | Function breakpoint — continue and pause when `func()` returns |
| `nfl` | List all function breakpoints with their indices |
| `nfd N` | Delete function breakpoint at index N |
| `c` | Continue — run until the next breakpoint |
| `q` | Quit |

```
(deb) > n foo
  Function breakpoint 0 set on foo()
(deb) > nfl
  Function breakpoints:
    [0] foo()
(deb) > nfd 0
  Function breakpoint 0 deleted
```

`n <func>` is useful when you want to skip over a function entirely and inspect state immediately after it returns — at that point `a` holds the return value and any local variables in the caller still reflect their pre-call state.

### Breakpoints

| Command | Description |
|---------|-------------|
| `b N` | Set a breakpoint at source line N |
| `bd N` | Delete breakpoint at index N |
| `bl` | List all breakpoints with their indices |

```
(deb) > b 12
  Breakpoint 0 set at line 12
(deb) > bl
  Breakpoints:
    [0] line 12
(deb) > bd 0
  Breakpoint 0 deleted
```

### Variables

| Command | Description |
|---------|-------------|
| `p name` | Print the current value of a variable (searches local scope first, then global, then enum/const) |

```
(deb) > p main
  main : Tk=Id Class=Fun Type=char Val=35005923456
(deb) > p printf
  printf : Tk=Id Class=Sys Type=int Val=33
(deb) > p if
  if : Tk=137 Class=0 Type=char Val=0
(deb) > p x
  x : Tk=Id Class=Loc Type=int Val=2
     -> 42  (local type=int val=bp[-1])
(deb) > p buf
  buf : Tk=Id Class=Glo Type=char* Val=140733234688
     -> "hello"  (global type=char* val=[140733234688])
(deb) > p MY_CONST
  MY_CONST = 7  [enum/const]
```

### Watch list

Watches are evaluated and printed automatically at every pause in the `WCH:` line.

| Command | Description |
|---------|-------------|
| `w name` | Add variable to the watch list (local scope → global → any function's local) |
| `wd N` | Delete watch at index N |
| `wl` | List all watches by index and name |

```
(deb) > w x
  Watch added x
(deb) > w buf
  Watch added buf
(deb) > wl
  Watches:
    [0] x
    [1] buf
(deb) > wd 0
  Watch 0 deleted
```

### Registers and stack

| Command | Description |
|---------|-------------|
| `r` | Show VM registers (`a`, `sp`, `bp`, `pc`) and the top 8 stack slots with their addresses and bp-relative offsets |

```
(deb) > r
  REG: a=42  sp=140731998800  bp=140731998840  pc=140731234568
  Stack:
    140731998800:[-5]  = 140731998824
    140731998808:[-4]  = 6158577176
    140731998816:[-3]  = 0
    140731998824:[-2]  = 140731234520
    140731998832:[-1]  = 140731998856
    140731998840:[ 0]B = 0
    140731998848:[ 1]  = 140731234530
    140731998856:[ 2]  = 3
```

(`B` marks the current frame pointer `bp`.)

### Source view

| Command | Description |
|---------|-------------|
| `src` | Show 5 lines of source around the current line (default context) |
| `src N` | Show N lines of source context around the current line |

```
(deb) > src 3
      9:  int x;
    >>10:  x = foo(2);
      11:  return x;
```

### Memory inspection — `i nnn`

`i nnn` takes a numeric address (decimal or `0x` hex) and identifies which memory segment it belongs to, then prints the relevant information.

| Segment matched | Output |
|-----------------|--------|
| **Symbol table** (`sym` array) | `SYM[idx]: Tk=Name Name="..." Class=Name Val=... Type=...` |
| **Code segment** (emitted bytecode) | `CODE[nnn]: OPNAME [operand]` |
| **Data segment** (global/static data) | `SYM: Tk=N Name="..." Class=Name Val=... Type=...  -> current_value` |
| **Stack** | `STK[nnn]: value  (offset from sp: ...  from bp: ...)` |
| **None of the above** (treated as source line number) | `SRC[nnn]: source text` |

#### Symbol table entry (`sym` range)

Displays the full sym entry for whichever identifier occupies that address.
`Tk` and `Class` are printed as token/class names (e.g. `Fun`, `Glo`, `Loc`):

```
(deb) > i 42488561680
  SYM[3]: Tk=Id Name="buf" Class=Glo Val=42488348168 Type=char*
```

#### Code segment (`e` range)

Shows the opcode (and operand if applicable) at that bytecode address:

```
(deb) > i 42488561680
  CODE[42488561680]: IMM 42488348168
(deb) > i 42488561688
  CODE[42488561688]: PSH
```

#### Data segment — global variable (`data` range)

When the address matches a global variable's storage location (`sym[Val]`), the sym entry is shown with `Tk` as a raw number, followed by the current value.
For integer globals the value is shown as a number; for `char*` globals the address and pointed-to string (with escape sequences) are shown:

```
(deb) > i 42488348160
  SYM: Tk=133 Name="x" Class=Glo Val=42488348160 Type=int
       [Val]=42
(deb) > i 42488348168
  SYM: Tk=133 Name="buf" Class=Glo Val=42488348168 Type=char*
       [[Val]]=[42488349200]="hello, world\n"
(deb) > i 42488349200
  DATA[42488349200]: 7303014251067469672 or "hello, w"
```

#### Stack (`sp`–`stk_hi` range)

```
(deb) > i 140731998808
  STK[140731998808]: 7  (offset from sp: 1  from bp: -4)
```

#### Source line (fallback)

If `nnn` does not match any memory segment, it is treated as a 1-based source line number:

```
(deb) > i 5
  SRC[5]:    printf("hello, world\n");
```

## Automatic pause display

Every time execution pauses (step, step-over, or breakpoint), c4deb prints:

```
=== [cycle=N] line=L in funcname() ===
  L:  <source line text>
   OP: OPNAME [operand]
  REG: a=...  sp=...  bp=...  pc=...
  STK: val0 val1 val2 val3
  WCH: varA=1  varB="hello"
(deb) >
```

- `STK:` shows the top 4 stack values; `B=` prefix marks the slot at `bp`.
- `WCH:` only appears when there is at least one active watch. Integer values are printed as numbers; `char*` values are printed as quoted strings with escape sequences.
- A `[Breakpoint at line N]` notice is printed when a breakpoint triggered the pause.

## VM instruction set

| Group | Opcodes |
|-------|---------|
| Memory | `LEA` `IMM` `LI` `LC` `SI` `SC` |
| Control flow | `JMP` `JSR` `BZ` `BNZ` `ENT` `ADJ` `LEV` |
| Stack | `PSH` |
| Arithmetic | `ADD` `SUB` `MUL` `DIV` `MOD` |
| Bitwise | `OR` `XOR` `AND` `SHL` `SHR` |
| Comparison | `EQ` `NE` `LT` `GT` `LE` `GE` |
| System calls | `OPEN` `READ` `CLOS` `PRTF` `MALC` `FREE` `MSET` `MCMP` `EXIT` |

## Example: debugging c4 interpreting hello.c

c4deb can debug the c4 compiler/interpreter itself while it processes another file.
This two-level session shows how to watch a `char*` variable (`pp`) that points into
a heap buffer allocated by the interpreted program — something that requires c4deb to
track `malloc` regions at runtime.

```sh
./c4deb -t c4.c hello.c
```

`hello.c`:
```c
#include <stdio.h>

int main()
{
  printf("hello, world\n");
  return 0;
}
```

At the first pause (c4's `main()` entry), set a watch on `pp` and a function breakpoint
on `next` (c4's lexer), then continue:

```
=== [cycle=1] line=333 in main() ===
  333:  int main(int argc, char **argv)
   OP: ENT  12
  ...
(deb) > w pp
  Watch added pp
(deb) > n next
  Function breakpoint 0 set on next()
(deb) > c
  [Function breakpoint: will pause after next() returns]

=== [cycle=436] line=83 in next() ===
  83:        return;
   OP: LEV
  REG: a=133  sp=...  bp=...  pc=...
  STK: ...
  WCH: pp="char else enum if int return sizeof while open read close printf"
(deb) >
```

What you see at the pause:

| Item | Meaning |
|------|---------|
| `line=83 in next()` | Paused at the `return` inside c4's `next()` — the first token has just been recognised |
| `OP: LEV` | The function is about to return; `a` holds the return value |
| `a=133` | Token id `133` = `Char` keyword (first keyword in c4's keyword string) |
| `WCH: pp="char else enum ..."` | `pp` is a `char*` local that points into the heap buffer where c4 loaded hello.c's keyword bootstrap string; c4deb displays it as a string because it tracks heap allocations made via `MALC` |

From here you can press `c` again to pause after the next call to `next()`, step through
c4's lexer one token at a time, or set a line breakpoint inside c4.c with `b N`.

## Based on

[c4](https://github.com/rswier/c4) by Robert Swierczek.
