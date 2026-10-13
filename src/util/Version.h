#pragma once

// The firmware version for display and logs. Development builds append the git branch and short SHA,
// which changes with every commit; it lives in Version.cpp alone (see scripts/git_branch.py) so a
// commit recompiles that one file, not the whole project.
const char* crosspointBuildVersion();
