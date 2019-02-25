project('ctsrunner', 'cpp')

thread_dep = dependency('threads')
boost_dep = dependency('boost', modules : ['filesystem', 'system'])

executable('vulkan-cts-runner',
           'src/vulkan-cts-runner.cc',
           dependencies : [thread_dep, boost_dep])
