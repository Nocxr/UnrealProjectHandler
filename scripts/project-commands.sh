#!/bin/sh
set -eu
action=$1
shift
project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
case "$action" in
  stop)
    exe=$1
    case "$exe" in /*) ;; *) exe="$project_root/$exe" ;; esac
    # Compare the full command path, rather than killing every process with this name.
    ps -axo pid=,comm= | while read -r pid command; do
      if [ "$command" = "$exe" ]; then kill "$pid"; fi
    done
    ;;
  clean)
    build=$1
    case "$build" in /*) target="$build" ;; *) target="$project_root/$build" ;; esac
    [ -e "$target" ] || exit 0
    [ ! -L "$target" ] || { echo 'Refusing to delete a symlink.' >&2; exit 1; }
    target=$(CDPATH= cd -- "$target" && pwd -P)
    case "$target" in "$project_root"/*) ;; *) echo 'Build directory must be inside the project.' >&2; exit 1 ;; esac
    case "${target##*/}" in .git|src|scripts|include|assets|third_party) echo 'Refusing to delete a source directory.' >&2; exit 1 ;; esac
    rm -rf -- "$target"
    ;;
  install|uninstall)
    exe=$1
    name=$2
    case "$name" in ''|*[!A-Za-z0-9_-]*) echo 'Invalid command name.' >&2; exit 1 ;; esac
    case "$exe" in /*) ;; *) exe="$project_root/$exe" ;; esac
    dest="$HOME/.local/bin/$name"
    if [ "$action" = install ]; then
      [ -x "$exe" ] || { echo "Executable missing: $exe" >&2; exit 1; }
      [ ! -e "$dest" ] || [ -L "$dest" ] || { echo 'Refusing to overwrite an unmanaged command.' >&2; exit 1; }
      mkdir -p -- "$HOME/.local/bin"
      ln -sfn -- "$exe" "$dest"
      echo "Registered $name -> $exe. Ensure ~/.local/bin is on PATH."
    else
      [ -L "$dest" ] || { echo 'No managed symlink to remove.'; exit 0; }
      [ "$(readlink "$dest")" = "$exe" ] || { echo 'Symlink belongs to another checkout.' >&2; exit 1; }
      rm -- "$dest"
    fi
    ;;
  *) echo 'Unknown action.' >&2; exit 1 ;;
esac