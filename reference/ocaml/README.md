# OCaml reference model

`model.ml` is a second, independent implementation of the MarketState state
machine. It exists so the C++ engine can be checked against **two** separately
written references (this one and `../python/model.py`) that both produce the same
canonical text. If a third language, with a different in-memory layout, agrees
byte-for-byte, a shared bug is far less likely.

## Build

```bash
./build.sh          # produces _build/model
```

`build.sh` finds `ocamlopt` on `PATH` or in `~/.local/ocaml/usr/bin`, and sets
`OCAMLLIB` when using a user-local compiler whose compiled-in stdlib path is
wrong. Requires OCaml 4.14+ (developed against 5.5).

## Run

Same CLI as the Python model, so it is a drop-in reference for the harness:

```bash
_build/model --events data/demo_clean.txt --state-out /tmp/ref.state
# prints a JSON summary to stdout
```

## Installing OCaml

Any of these work; `build.sh` auto-detects the first two.

**System package (needs root):**

```bash
# Arch
sudo pacman -S ocaml
# Debian/Ubuntu
sudo apt-get install ocaml-nox
```

**opam (user-level):**

```bash
# https://opam.ocaml.org/doc/Install.html, then:
opam init --disable-sandboxing -y
eval "$(opam env)"
opam install -y ocamlfind
```

**User-local extraction (no root), as used to develop this model:**

```bash
curl -fsSL -o /tmp/ocaml.pkg.tar.zst \
  "https://geo.mirror.pkgbuild.com/extra/os/x86_64/ocaml-5.5.0-1-x86_64.pkg.tar.zst"
mkdir -p ~/.local/ocaml
bsdtar -xf /tmp/ocaml.pkg.tar.zst -C ~/.local/ocaml
export PATH="$HOME/.local/ocaml/usr/bin:$PATH"
export OCAMLLIB="$HOME/.local/ocaml/usr/lib/ocaml"
ocamlopt -version
```

`build.sh` already knows this layout, so no further configuration is needed.
