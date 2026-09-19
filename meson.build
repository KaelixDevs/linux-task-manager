project('linux-task-manager', 'c',
  version: '0.3.1',
  default_options: ['warning_level=2', 'c_std=c17'])

gtk = dependency('gtk4', version: '>=4.12')

executable('linux-task-manager',
  'src/main.c',
  dependencies: gtk,
  install: true)

install_data('data/io.github.linuxtaskmanager.TaskManager.desktop',
  install_dir: join_paths(get_option('datadir'), 'applications'))

install_data('data/io.github.linuxtaskmanager.TaskManager.svg',
  install_dir: join_paths(get_option('datadir'), 'icons', 'hicolor', 'scalable', 'apps'))

install_data('data/io.github.linuxtaskmanager.TaskManager.metainfo.xml',
  install_dir: join_paths(get_option('datadir'), 'metainfo'))
