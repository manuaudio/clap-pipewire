test_plugin = shared_module('clap-pipewire-test',
  'test-plugin/test-plugin.c',
  name_prefix : '',
  name_suffix : 'clap',
  dependencies : [clap_dep, m_dep],
  c_args : ['-fvisibility=default'])

test_args = [
  '-DTEST_PLUGIN_PATH="@0@"'.format(test_plugin.full_path()),
  '-DSPA_PLUGIN_PATH="@0@"'.format(clap_spa.full_path()),
]

test_host = executable('test-host', 'test-host.c',
  dependencies : [host_dep, thread_dep, m_dep],
  c_args : test_args)

test_spa = executable('test-spa', 'test-spa.c',
  include_directories : include_directories('../src'),
  dependencies : [spa_dep, dl_dep, m_dep],
  c_args : test_args,
  export_dynamic : true)

test('host core', test_host, depends : [test_plugin])
test('spa plugin', test_spa, depends : [test_plugin, clap_spa])
test('clap-pw-info', clap_pw_info,
  args : [test_plugin.full_path(), 'org.clap-pipewire.test.gain'],
  depends : [test_plugin])
test('clap-pw-info convert', clap_pw_info,
  args : [test_plugin.full_path(), 'org.clap-pipewire.test.gain', 'Mode=Hot', 'Gain_dB=-6'],
  depends : [test_plugin])
test('clap-pw-info check', clap_pw_info,
  args : ['-c', test_plugin.full_path(), 'org.clap-pipewire.test.gain'],
  depends : [test_plugin])
