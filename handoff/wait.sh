#!/bin/sh
# usage: handoff/wait.sh requests|results  — blocks until the other
# side pushes something under handoff/<arg>/, then prints the new files
#
# Branch: $HANDOFF_BRANCH if set, else the upstream of the checked-out
# branch, else main (the repo started without main; see CLAUDE.md).
dir=handoff/$1
br=${HANDOFF_BRANCH:-$(git rev-parse --abbrev-ref --symbolic-full-name '@{u}' 2>/dev/null | sed 's#^origin/##')}
[ -n "$br" ] || br=main
last=$(git rev-parse HEAD)
while :; do
  git fetch -q origin "$br"
  if [ -n "$(git diff --name-only "$last" "origin/$br" -- "$dir")" ]; then
    git pull -q --rebase origin "$br"
    git diff --name-only "$last" HEAD -- "$dir"; exit 0
  fi
  sleep 60
done
