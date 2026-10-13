#!/usr/bin/env bash
# Build the OCaml reference model into ./_build/model.
#
# OCaml can come from a system package (ocamlopt on PATH) or from a user-local
# extraction (e.g. ~/.local/ocaml from the Arch package). When the latter is
# used the compiler's compiled-in stdlib path is wrong, so we set OCAMLLIB.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if ! command -v ocamlopt >/dev/null 2>&1; then
  for p in "$HOME/.local/ocaml/usr/bin" /usr/bin; do
    if [ -x "$p/ocamlopt" ]; then
      export PATH="$p:$PATH"
      break
    fi
  done
fi

if ! command -v ocamlopt >/dev/null 2>&1; then
  echo "error: ocamlopt not found. Install OCaml or see reference/ocaml/README.md" >&2
  exit 1
fi

if [ -z "${OCAMLLIB:-}" ]; then
  for d in "$HOME/.local/ocaml/usr/lib/ocaml" /usr/lib/ocaml; do
    if [ -f "$d/stdlib.cmi" ]; then
      export OCAMLLIB="$d"
      break
    fi
  done
fi

mkdir -p "$HERE/_build"
cp "$HERE/model.ml" "$HERE/_build/model.ml"
( cd "$HERE/_build" && ocamlopt -o model model.ml )
echo "built $HERE/_build/model"
