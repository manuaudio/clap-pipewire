project('clap-pipewire', 'c',
  version : '0.1.0',
  license : 'MIT',
  meson_version : '>= 0.63.0',
  default_options : [
    'c_std=gnu11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ])

cc = meson.get_compiler('c')
add_project_arguments(
  cc.get_supported_arguments([
    '-Wno-unused-parameter',
    '-Wno-missing-field-initializers',
    '-fvisibility=hidden',
  ]),
  language : 'c')

spa_dep = dependency('libspa-0.2')
clap_dep = dependency('clap', version : '>= 1.2.0')
dl_dep = cc.find_library('dl', required : false)
thread_dep = dependency('threads')
m_dep = cc.find_library('m', required : false)

# The plugin talks to libspa-filter-graph.so through the uninstalled
# audio-plugin.h ABI (vendored from 1.6.8). Warn when building for another
# PipeWire series.
pw_dep = dependency('libpipewire-0.3', required : false)
if pw_dep.found()
  pw_version = pw_dep.version()
  if not pw_version.version_compare('>= 1.6.0') or not pw_version.version_compare('< 1.9.0')
    warning('PipeWire @0@ found; this plugin targets the 1.6 filter-graph plugin ABI'.format(pw_version))
  endif
endif

# Where the plugin goes. filter-graph looks for
#   <dir in SPA_PLUGIN_DIR>/filter-graph/libspa-filter-graph-plugin-clap.so
spa_plugindir = get_option('spa-plugindir')
if spa_plugindir == ''
  spa_plugindir = get_option('prefix') / get_option('libdir') / 'spa-0.2'
endif
system_spa_plugindir = spa_dep.get_variable(pkgconfig : 'plugindir', default_value : '/usr/lib/spa-0.2')
datadir = get_option('prefix') / get_option('datadir') / 'clap-pipewire'

cdata = configuration_data()
cdata.set_quoted('CPH_VERSION', meson.project_version())
# the PipeWire series whose audio-plugin.h is vendored in src/fga
cdata.set_quoted('FGA_ABI_SERIES', '1.6.')
configure_file(output : 'config.h', configuration : cdata)
add_project_arguments('-include', meson.current_build_dir() / 'config.h', language : 'c')

host_lib = static_library('clap-host',
  'src/clap-host.c',
  dependencies : [clap_dep, dl_dep, thread_dep, m_dep],
  pic : true)
host_dep = declare_dependency(
  link_with : host_lib,
  include_directories : include_directories('src'),
  dependencies : [clap_dep, dl_dep, thread_dep, m_dep])

clap_spa = shared_library('spa-filter-graph-plugin-clap',
  'src/plugin_clap.c',
  dependencies : [spa_dep, host_dep],
  # plugins can leave callbacks queued on PipeWire's loops; never unmap us
  link_args : cc.get_supported_link_arguments(['-Wl,-z,nodelete']),
  install : true,
  install_dir : spa_plugindir / 'filter-graph')

clap_pw_info = executable('clap-pw-info',
  'src/clap-pw-info.c',
  dependencies : [host_dep],
  install : true)

# helper that runs a filter-chain config in its own PipeWire client with
# this plugin on SPA_PLUGIN_DIR
hdata = configuration_data()
hdata.set('SPA_PLUGINDIR', spa_plugindir)
hdata.set('SYSTEM_SPA_PLUGINDIR', system_spa_plugindir)
hdata.set('DATADIR', datadir)
configure_file(input : 'tools/clap-pw-host.in', output : 'clap-pw-host',
  configuration : hdata, install : true,
  install_dir : get_option('bindir'), install_mode : 'rwxr-xr-x')

udata = configuration_data()
udata.set('BINDIR', get_option('prefix') / get_option('bindir'))
configure_file(input : 'data/clap-pw-host@.service.in', output : 'clap-pw-host@.service',
  configuration : udata, install : get_option('systemd-user-unit'),
  install_dir : get_option('prefix') / get_option('datadir') / 'systemd' / 'user')

install_data(
  'examples/clap-sink.conf',
  'examples/clap-smart-filter.conf',
  install_dir : datadir / 'examples')

if get_option('tests')
  subdir('tests')
endif

summary({
  'SPA plugin dir' : spa_plugindir / 'filter-graph',
  'System SPA dir' : system_spa_plugindir,
  'PipeWire' : pw_dep.found() ? pw_dep.version() : 'not found (headers only)',
  'CLAP' : clap_dep.version(),
})
