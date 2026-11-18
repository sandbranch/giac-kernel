# giac-kernel

A Jupyter kernel for [Giac](https://www-fourier.univ-grenoble-alpes.fr/~parisse/giac.html),
the computer algebra system behind Xcas and the HP Prime.

It is one small native program linked against libgiac: no Python and no
Jupyter installation needed. Any client that speaks the Jupyter kernel
protocol can use it. It was written for [jotter](https://github.com/samox73/jotter),
the terminal notebook, and works the same in JupyterLab or VS Code.

```
jotter --kernel giac
```

Cells are plain Giac:

```
factor(x^4-1)                 → (x-1)·(x+1)·(x²+1), typeset
desolve(y''+y=0,y)            → c₀·cos x + c₁·sin x
int(sin(x)/x,x,0,inf)         → π/2
plot([x^2,x^3],x=-2..2)       → an SVG plot
a:=3:;                        → assigns, shows nothing (Xcas convention)
?factor                       → help on factor
```

## What it does

- Results as plain text and LaTeX (`text/latex`); 2D graphics as SVG, fitted
  to the plot.
- 3D surfaces and curves (`plotfunc(x^2-y^2,[x,y])`, 3D `plotparam`) as a
  static shaded SVG picture. libgiac has no 3D export, so the kernel samples
  the parametrization Giac returns and draws it itself (`src/plot3d.cc`).
  Other 3D objects (points, spheres, planes) are not drawn yet.
- Output of `print()` and Giac's warnings appear as the cell's output.
- Syntax and evaluation errors are reported as errors, with Giac's message.
- Tab completion over Giac's ~1900 commands and your own variables.
- Help: `?command` in a cell, or the client's docs key (Shift+Tab in jotter).
- Interrupting a long computation, by signal or by `interrupt_request`.
- Several statements in one cell (`a:=3;b:=a+1`) show their results as a
  sequence (`3,4`), the same as Giac's own console.

## Syntax highlighting

No editor or notebook front end ships a Giac grammar, so the kernel brings
one: `syntax/Giac.sublime-syntax`, installed as
`/usr/share/giac-kernel/Giac.sublime-syntax`. It colours keywords, Giac's own
~1900 commands (taken from libgiac by `tools/gen-syntax.cc`), your functions,
numbers, strings and comments, and reads `y''` as derivative primes.

It is in the sublime-syntax format that syntect-based tools read: copy it to
`~/.config/bat/syntaxes/` for bat, or to `~/.config/jotter/syntaxes/` for a
jotter that loads user grammars. Front ends that only take a hint get one in
`kernel_info`: `pygments_lexer` "matlab", the closest existing grammar.

## Install

Build the Debian package in a clean container (needs Docker):

```
tools/build-deb.sh                  # Ubuntu 26.04
tools/build-deb.sh debian:trixie    # Debian 13
sudo apt install ./dist/ubuntu-26.04/giac-kernel_*.deb
```

It installs `/usr/bin/giac-kernel` and the kernelspec
`/usr/share/jupyter/kernels/giac/kernel.json`, and depends only on libgiac,
libzmq and OpenSSL's libcrypto.

## Build from source

```
sudo apt install cmake pkgconf libgiac-dev libzmq3-dev libssl-dev nlohmann-json3-dev
cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

The test (`tests/protocol_test.cc`) starts the kernel with a signed connection
file and drives it over ZeroMQ like a real client: execution, errors, plots,
completion, help, both kinds of interrupt and shutdown.

To try a local build in jotter without installing it, point `JUPYTER_PATH` at
a directory holding `kernels/giac/kernel.json` with `argv[0]` set to
`build/giac-kernel`.

## How it fits together

- `src/wire.cc`: the Jupyter wire protocol (version 5.3): connection file,
  HMAC-SHA256 signing, multipart framing on libzmq.
- `src/engine.cc`: everything that touches Giac: parse, evaluate, LaTeX, 2D SVG,
  help and completion.
- `src/plot3d.cc`: 3D surfaces and curves to SVG.
- `src/main.cc`: the five sockets. The main thread evaluates; a heartbeat
  thread and a control thread keep answering, so interrupt and shutdown get
  through during a long computation.

Built and tested against Giac 1.9.0 as packaged in Debian 13 and Ubuntu 26.04.

## License

GPL-3.0-or-later, the same as Giac. See `LICENSE`.
