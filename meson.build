project('VideoStreamsWsi', 'cpp',
	version : '0.1',
	default_options : ['warning_level=3', 'cpp_std=c++2a']
)

add_global_arguments('-Wno-missing-field-initializers', language : 'cpp')

compiler = meson.get_compiler('cpp')


shared_library('VideoStreamsWsi',
	[
		'Layer.cpp',
		'Wsi.cpp',
	],
	name_prefix: '',
	include_directories: [
		'/boot/system/develop/headers/private/shared',
	],
	dependencies: [
		compiler.find_library('be'),
		dependency('libvideostreams'),
		dependency('libdrm'),
	],
	gnu_symbol_visibility: 'hidden',
	install_dir: 'add-ons/vulkan/implicit_layer.d',
	install: true
)

install_data(
	'VideoStreamsWsi.json',
	install_dir: 'add-ons/vulkan/implicit_layer.d',
)
