project('ctsrunner', ['c', 'cpp'])

thread_dep = dependency('threads')
boost_dep = dependency('boost', modules : ['filesystem', 'system'])
vulkan_dep = dependency('vulkan')

executable('vulkan-cts-runner',
           'src/vulkan-cts-runner.cc',
           dependencies : [thread_dep, boost_dep])

executable('hang-detection',
           'src/hang_detection.c',
           dependencies: [vulkan_dep])
