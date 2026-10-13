#!/usr/bin/env bash
here="$(cd -- "$(dirname -- "$0")" && pwd)"
exec python "$here/../../executar_cache.py" --threads 2
