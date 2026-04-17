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
| `c` | Continue — run until the next breakpoint |
| `q` | Quit |

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
(deb) > p x
  x = 42 (42)  [local bp[-1]]
(deb) > p buf
  buf = 140732001234 (140732001234)  [global]
(deb) > p MY_CONST
  MY_CONST = 7  [enum/const]
```

### Watch list

Watches are evaluated and printed automatically at every pause in the `WCH:` line.

| Command | Description |
|---------|-------------|
| `w name` | Add variable to the watch list (local scope → global → any function's local) |
| `wd N` | Delete watch at index N |
| `wl` | List all watches with their current values |

```
(deb) > w x
  Watch added (local): x  bp[-1]  type=int
(deb) > w buf
  Watch added (global): buf
(deb) > wl
  Watches:
    [0] x = 42  (local bp[-1])
    [1] buf = 140732001234  (global)
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
    140731998800:[-5] = 140731998824
    140731998808:[-4] = 6158577176
    ...
    140731998840:[ 0]B= 0         ← B marks bp
```

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

```
(deb) > i nnn
```

| Segment matched | Output |
|-----------------|--------|
| **Symbol table** (`sym` array) | `SYM[idx]: Tk=... Name=... Class=... Type=... Val=...` |
| **Code segment** (emitted bytecode) | `CODE[nnn]: OPNAME [operand]` |
| **Data segment** (global/static data) | `SYM: Tk=... Name=... Class=... Type=... Val=...  -> current_value` |
| **Stack** | `STK[nnn]: value  (offset from sp: ...  from bp: ...)` |
| **None of the above** (treated as source line number) | `SRC[nnn]: source text` |

#### Symbol table entry (`sym` range)

Displays the full sym entry for whichever identifier occupies that address:

```
(deb) > i 42488561680
  SYM[3]: Tk=133 Name=buf Class=Glo Type=ptr Val=42488348160
```

#### Code segment (`e` range)

Shows the opcode (and operand if applicable) at that bytecode address:

```
(deb) > i 42488561680
  CODE[42488561680]: IMM  42488348160
```

#### Data segment — global variable (`data` range)

When the address matches a global variable's storage location (`sym[Val]`), the sym entry is shown together with the variable's current value.
For integer globals the value is shown as a number; for pointer/char globals the pointed-to string is shown with escape sequences so control characters never corrupt the terminal:

```
(deb) > i 42488348160
  SYM: Tk=133 Name=x   Class=Glo Type=int Val=42488348160  -> 42
  SYM: Tk=133 Name=buf Class=Glo Type=ptr Val=42488348168  -> 42488349200="hello, world\n"
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

Every time execution pauses, c4deb prints:

```
=== [cycle=N] line=L in funcname() ===
  L:  <source line text>
   OP: OPNAME [operand]
  REG: a=...  sp=...  bp=...  pc=...
  STK: val0 val1 val2 val3
  WCH: varA=1  varB=42
(deb) >
```

The `WCH:` line only appears when there is at least one active watch.
A `[Breakpoint at line N]` notice is printed when a breakpoint triggered the pause.

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

## Based on

[c4](https://github.com/rswier/c4) by Robert Swierczek.
