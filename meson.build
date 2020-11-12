project('flextop', 'c')

deps = [
  dependency('glib-2.0', required : true),
  dependency('gio-2.0', required : true),
  dependency('gtk+-3.0', required : true),
]

utils = static_library('flextop-utils', ['src/flextop-utils.c'], dependencies : deps)

bins = ['flextop-init', 'xdg-desktop-menu', 'xdg-icon-resource']
foreach bin : bins
  executable(bin, ['src/@0@.c'.format(bin)], link_with : [utils], dependencies : deps,
             install : true)
endforeach
