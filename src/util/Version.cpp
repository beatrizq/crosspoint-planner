#include "Version.h"

// Development builds: scripts/git_branch.py writes the branch and SHA into this header and defines
// CROSSPOINT_BUILD_FROM_GIT (a constant flag), so the header is the only thing a commit changes.
#if defined(CROSSPOINT_BUILD_FROM_GIT)
#include "VersionBuild.generated.h"
#endif

const char* crosspointBuildVersion() {
#if defined(CROSSPOINT_BUILD_FROM_GIT)
  return CROSSPOINT_BUILD_VERSION;
#else
  return CROSSPOINT_VERSION;  // release builds set it in platformio.ini
#endif
}
