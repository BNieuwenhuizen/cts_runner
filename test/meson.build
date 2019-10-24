deqp_mock = executable('deqp-mock',
                       'deqp-mock.cc',
                       install: false)

tests = [
  'pass',
  'fail',
  'crash',
  'missingtest',
]

wrap = find_program('cts_runner-test-wrapper.sh')

foreach t : tests
  if (t == 'pass')
    expected_exit_code = '0'
  else
    expected_exit_code = '1'
  endif

  test('deqp-' + t,
    wrap,
    args: [
      # Args to the wrapper
      t,
      cts_runner.full_path(),
      join_paths(meson.current_source_dir(), 'deqp-' + t + '-expected.txt'),
      expected_exit_code,

      # Args to cts_runner
      '--deqp',
      deqp_mock.full_path(),
      '--caselist',
      join_paths(meson.current_source_dir(), 'deqp-caselist.txt'),
    ],
  )
endforeach

test('deqp-big-caselist',
  wrap,
  args: [
    'pass',
    cts_runner.full_path(),
    join_paths(meson.current_source_dir(), 'deqp-big-expected.txt'),
    '0',

    '--deqp',
    deqp_mock.full_path(),
    '--caselist',
    join_paths(meson.current_source_dir(), 'deqp-big-caselist.txt'),
  ],
)
