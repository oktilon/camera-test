project(
    'camera-test', 'c',
    version : '0.1.0',
    meson_version : '>= 0.56.0')

prj_name = meson.project_name()
prj_ver = meson.project_version()
dbPath = get_option('doorbell_project')
copyPath = ''
fs = import('fs')

if dbPath == ''
    curProj = meson.project_source_root()
    dbPath = fs.parent(curProj) / 'doorbell-v2'
endif

if fs.is_dir(dbPath)
    copyPath = dbPath / 'deb.defigo' / 'usr' / 'bin' 
endif

doInstall = copyPath != '' and fs.is_dir(copyPath)

# Set config
conf_data = configuration_data()
conf_data.set('version', prj_ver)

configure_file(
    input : 'config.hh',
    output : 'config.h',
    configuration : conf_data
)

# Includes and sources
inc = include_directories('src')
src = [
      'src/app.c'
    , 'src/main.c'
]

# Dependencies
deps = [
    # dependency('gtk+-3.0', static: true)
    dependency('gtk+-3.0')
]

# Create executable
executable(
      prj_name
    , sources             : src
    , include_directories : inc
    , dependencies        : deps
    , install             : doInstall
    , install_dir         : copyPath
)
