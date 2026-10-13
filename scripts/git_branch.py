"""
PlatformIO pre-build script: give development environments a version string with the git branch
and short SHA, e.g.  1.1.0-dev-feat-kosync-xpath-05c6cf8

The string is written to src/util/VersionBuild.generated.h and read only by src/util/Version.cpp
(crosspointBuildVersion()). It must not become a compiler flag: a flag that changes with every
commit makes every file recompile after every commit. CROSSPOINT_VERSION stays a constant
("<base>-dev") for the places that need a compile-time literal, and CROSSPOINT_BUILD_FROM_GIT tells
Version.cpp to read the generated header.
Release environments are unaffected; they set CROSSPOINT_VERSION in the ini.
"""

import configparser
import os
import subprocess
import sys


def warn(msg):
    print(f'WARNING [git_branch.py]: {msg}', file=sys.stderr)


def run_git_value(project_dir, args, label):
    try:
        value = subprocess.check_output(
            ['git', *args],
            text=True, stderr=subprocess.PIPE, cwd=project_dir
        ).strip()
        # Strip characters that would break a C string literal
        return ''.join(c for c in value if c not in '"\\')
    except FileNotFoundError:
        warn(f'git not found on PATH; {label} suffix will be "unknown"')
        return 'unknown'
    except subprocess.CalledProcessError as e:
        warn(
            f'git command failed (exit {e.returncode}): '
            f'{e.stderr.strip()}; {label} suffix will be "unknown"'
        )
        return 'unknown'
    except OSError as e:
        warn(
            f'OS error reading git {label}: {e}; '
            f'{label} suffix will be "unknown"'
        )
        return 'unknown'
    except Exception as e:  # pylint: disable=broad-exception-caught
        warn(
            f'Unexpected error reading git {label}: {e}; '
            f'{label} suffix will be "unknown"'
        )
        return 'unknown'


def get_git_branch(project_dir):
    branch = run_git_value(
        project_dir, ['rev-parse', '--abbrev-ref', 'HEAD'], 'branch'
    )
    # Detached HEAD has no branch name.
    if branch == 'HEAD':
        return 'detached'
    return branch


def get_git_short_sha(project_dir):
    return run_git_value(
        project_dir, ['rev-parse', '--short', 'HEAD'], 'short SHA'
    )


def get_base_version(project_dir):
    ini_path = os.path.join(project_dir, 'platformio.ini')
    if not os.path.isfile(ini_path):
        warn(f'platformio.ini not found at {ini_path}; base version will be "0.0.0"')
        return '0.0.0'
    config = configparser.ConfigParser()
    config.read(ini_path, encoding='utf-8')
    if not config.has_option('crosspoint', 'version'):
        warn('No [crosspoint] version in platformio.ini; base version will be "0.0.0"')
        return '0.0.0'
    return config.get('crosspoint', 'version')


def inject_version(env):
    # Only applies to development environments; release envs set the
    # version via build_flags in platformio.ini and are unaffected.
    if env['PIOENV'] not in ('default', 'sticky'):
        return

    project_dir = env['PROJECT_DIR']
    base_version = get_base_version(project_dir)
    branch = get_git_branch(project_dir)
    short_sha = get_git_short_sha(project_dir)
    version_string = f'{base_version}-dev-{branch}-{short_sha}'

    header_path = os.path.join(project_dir, 'src', 'util', 'VersionBuild.generated.h')
    header = f'#pragma once\n#define CROSSPOINT_BUILD_VERSION "{version_string}"\n'
    # Rewrite only when the string changed, so an unchanged commit recompiles nothing.
    try:
        with open(header_path, 'r', encoding='utf-8') as handle:
            unchanged = handle.read() == header
    except OSError:
        unchanged = False
    if not unchanged:
        with open(header_path, 'w', encoding='utf-8', newline='\n') as handle:
            handle.write(header)

    env.Append(CPPDEFINES=[
        ('CROSSPOINT_VERSION', f'\\"{base_version}-dev\\"'),
        ('CROSSPOINT_BUILD_FROM_GIT', '1'),
    ])
    print(f'CrossPoint build version: {version_string}')


# PlatformIO/SCons entry point — Import and env are SCons builtins injected at runtime.
# When run directly with Python (e.g. for validation), a lightweight fake env is used
# so the git/version logic can be exercised without a full build.
try:
    Import('env')           # noqa: F821  # type: ignore[name-defined]
    inject_version(env)     # noqa: F821  # type: ignore[name-defined]
except NameError:
    class _Env(dict):
        def Append(self, **_):
            # Validation mode does not need SCons state; this stub only satisfies inject_version().
            return None

    _project_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    inject_version(_Env({'PIOENV': 'default', 'PROJECT_DIR': _project_dir}))
