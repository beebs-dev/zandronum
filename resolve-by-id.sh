#!/bin/bash
set -euo pipefail
resolve_by_id() (
  local DATA_ROOT="$1"
  local id="$2"
  
  cd "$DATA_ROOT"

  # 1) Old behavior: file named exactly the ID
  if [[ -f "$id" ]]; then
    printf '%s\n' "$id"
    return 0
  fi

  # 2) New behavior: ID.<ext> (preserve extension)
  local matches=()
  # Null-delimited to be safe with odd filenames; IDs won't have spaces, but extensions might.
  while IFS= read -r -d '' f; do
    matches+=("$f")
  done < <(find . -maxdepth 1 -type f -name "${id}.*" -print0)

  if (( ${#matches[@]} == 0 )); then
    echo "ERROR: Could not resolve file for id=$id in $DATA_ROOT (looked for '$id' or '${id}.*')" >&2
    ls -al "$DATA_ROOT" >&2 || true
    return 1
  fi
  if (( ${#matches[@]} > 1 )); then
    echo "ERROR: Ambiguous files for id=$id (more than one match):" >&2
    printf '  %s\n' "${matches[@]}" >&2
    return 1
  fi

  # strip leading ./ from find output
  printf '%s\n' "${matches[0]#./}"
)
resolve_by_id "$@"