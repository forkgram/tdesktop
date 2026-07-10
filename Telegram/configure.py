'''
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
'''
import sys, os, re

sys.dont_write_bytecode = True
scriptPath = os.path.dirname(os.path.realpath(__file__))
sys.path.append(scriptPath + '/../cmake')
import run_cmake
sys.path.append(scriptPath + '/build')
import qt_version

executePath = os.getcwd()
def finish(code):
    global executePath
    os.chdir(executePath)
    sys.exit(code)

def error(message):
    print('[ERROR] ' + message)
    finish(1)

if sys.platform == 'win32' and 'COMSPEC' not in os.environ:
    error('COMSPEC environment variable is not set.')

scriptName = os.path.basename(scriptPath)

arguments = sys.argv[1:]
officialTarget = ''
officialTargetFile = scriptPath + '/build/target'
if os.path.isfile(officialTargetFile):
    with open(officialTargetFile, 'r') as f:
        for line in f:
            officialTarget = line.strip()

arch = ''
if officialTarget in ['win', 'uwp']:
    arch = 'x86'
elif officialTarget in ['win64', 'uwp64']:
    arch = 'x64'
elif officialTarget in ['winarm', 'uwparm']:
    arch = 'arm'
if not qt_version.resolve(arch):
    error('Unsupported platform.')

if 'qt6' in arguments:
    arguments.remove('qt6')

# Support command line API credentials (newer format)
api_id = ''
api_hash = ''
remaining_args = []
for arg in arguments:
    if arg.startswith('-DTDESKTOP_API_ID='):
        api_id = arg.split('=', 1)[1]
    elif arg.startswith('-DTDESKTOP_API_HASH='):
        api_hash = arg.split('=', 1)[1]
    else:
        remaining_args.append(arg)
arguments = remaining_args

if officialTarget != '':
    # Official build: read from file
    officialApiIdFile = scriptPath + '/../../DesktopPrivate/custom_api_id.h'
    if not os.path.isfile(officialApiIdFile):
        error('DesktopPrivate/custom_api_id.h not found.')
    with open(officialApiIdFile, 'r') as f:
        for line in f:
            apiIdMatch = re.search(r'ApiId\s+=\s+(\d+)', line)
            apiHashMatch = re.search(r'ApiHash\s+=\s+"([a-fA-F\d]+)"', line)
            if apiIdMatch:
                api_id = apiIdMatch.group(1)
            elif apiHashMatch:
                api_hash = apiHashMatch.group(1)

# Add API credentials as cmake defines
if api_id:
    arguments.append('-DTDESKTOP_API_ID=' + api_id)
if api_hash:
    arguments.append('-DTDESKTOP_API_HASH=' + api_hash)

# Determine architecture from officialTarget or detect from arguments
if not arch:
    # Try to detect from arguments (newer workflow format)
    for arg in arguments:
        if arg in ['x86', 'x64', 'arm']:
            arch = arg
            break

# Pass arch to run_cmake for proper -A flag
if arch and arch not in arguments:
    arguments.append(arch)

finish(run_cmake.run(scriptName, arguments))
