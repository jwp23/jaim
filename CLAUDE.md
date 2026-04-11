# jaim contributor notes

This file is auto-loaded by Claude Code at the start of every session
in this repo. Rules below apply in addition to Claude Code's
built-in coding guidelines.

## License compliance (GPL-3.0-or-later)

This repo is GPL-3.0-or-later; see `COPYING`. Per GPL3 §5(a), every
modified file must carry a "prominent notice stating that you
changed the files and the date of any change."

### When modifying existing source files (`.cc`, `.h`)

- **Never strip** existing `Copyright` or `Modified` lines from the
  file header block. They are the license audit trail.
- For **substantive** changes (new features, bug fixes, behavior
  changes, new entries in a config list), append a new line inside
  the header comment, just before the closing `*/`:

  ```
   * Modified YYYY by <Your Name>: <short summary> (<bead-id>).
  ```

  where `YYYY` is the current year. Match the existing style: leading
  ` * `, sentence case, period at end. If the summary wraps, indent
  continuation lines with ` *   ` (two spaces after the star).
- **Trivial** changes (typo fixes, whitespace, comment-only edits)
  do NOT need a new Modified line — git log is sufficient.

### When creating new source files

- `.cc` / `.h`: copy the full license block from `jaim.h` verbatim.
  Update the `Copyright (C) YYYY <Your Name>` lines for your
  authorship. Drop the pre-existing "Modified" lines — those apply
  only to files that were in the original jai upstream.
- Shell scripts and other text files: add near the top (after the
  shebang for scripts):

  ```
  # SPDX-License-Identifier: GPL-3.0-or-later
  # Copyright (C) YYYY <Your Name>
  ```

- Non-code assets (README, images, config files loaded as data) do
  not require headers.

### Verifying compliance

Run the check before committing any source changes:

```sh
sh scripts/check-gpl3.sh
```

It scans the working tree against `HEAD` and flags:
- new `.cc` / `.h` / `.sh` files without SPDX or GPL notice
- modified `.cc` / `.h` files without a current-year `Modified` line

Exits 0 on clean, nonzero on violations. Pass `--staged` to check
the git staging area instead of the working tree.

### One-time setup: install the pre-commit hook

The hook lives in `.githooks/` (tracked in the repo, unlike
`.git/hooks/`). Enable it once per clone with:

```sh
git config core.hooksPath .githooks
```

After that, `git commit` automatically runs `check-gpl3.sh --staged`
and blocks commits that violate the rules. Skip the hook for a
specific commit with `git commit --no-verify` — but only when you
have a real reason to.

## Beads workflow

This repo uses beads (`bd`) for task tracking. See the SessionStart
hook output at the top of each session for the full command
reference. Short version: `bd create` before writing code, `bd
update <id> --claim` when starting, `bd close <id>` when done.
Reference the bead ID in commit messages and in new Modified lines.

## Build

```sh
make          # builds ./jaim in the source tree
make install  # installs to ~/.local/bin by default
```

Tests are shell scripts under `tests/`; run them individually with
`sh tests/<name>.sh` from the repo root. The autotools test harness
from upstream jai is not wired into the macOS GNUmakefile build.
