project('ctsrunner', ['c', 'cpp'],
	default_options: 'cpp_std=c++17')
cpp = meson.get_compiler('cpp')

thread_dep = dependency('threads')
vulkan_dep = dependency('vulkan')
fs_dep = cpp.find_library('stdc++fs')

cts_runner = executable('cts-runner',
           'src/cts-runner.cc',
           dependencies : [thread_dep, fs_dep],
           install : true)

executable('hang-detection',
           'src/hang_detection.c',
           dependencies: [vulkan_dep])

subdir('test')
