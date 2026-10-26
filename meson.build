# ── sdk/xkit — UI toolkit subcomponent ───────────────────────────────────────
# Included via subdir('sdk/xkit') from the root meson.build.
# Expects the following variables to exist in the parent scope (set in root):
#   xrender_dep, thread_dep, gbm_dep, egl_dep, glesv2_dep, ffmpeg_deps, gtest_dep
# Exports:
#   libxkit   — static_library target
#   xkit_dep  — declare_dependency wrapping libxkit + xrender_dep

xkit_srcs = files(
  'src/engine.cc',
  'src/widget.cc',
  'src/application.cc',
  'src/xbus_client.cc',
  'src/clipboard.cc',
  'src/translator.cc',
  'src/a11y_phrasing.cc',
  'src/dialog.cc',
  'src/menu.cc',
  'src/toast.cc',
  'src/audio_player.cc',
  'src/gpu_surface.cc',
  'src/video_surface.cc',
  # ipc_socket and xbus_message_pool are xserver primitives required by xbus_client
  '../../core/xserver/src/ipc_socket.cc',
  '../../core/xserver/src/xbus_message_pool.cc',
  # crash handler: application.cc installs it, so it lives in libxkit
  '../../core/xserver/src/crash_handler.cc',
)

execinfo_dep = meson.get_compiler('cpp').find_library('execinfo')  # backtrace() for crash_handler

# Include dirs (paths relative to sdk/xkit/)
xkit_inc = include_directories(
  'include',
  '../../include',
  '../../sdk/xrender/include',
  '../../core/xserver/include',
  '/usr/local/include/libdrm',
)

libxkit = static_library('xkit',
  xkit_srcs,
  include_directories : xkit_inc,
  # Do not link_with libxrender here; libfreebsdx-ui handles the final link
  # so we avoid pulling xrender objects into both libxkit and libxrender.
  # freetype_dep is needed because xrender headers (font_manager.h) are
  # transitively included by xkit headers via render_node.h → display_list.h.
  dependencies        : [thread_dep, gbm_dep, egl_dep, glesv2_dep, freetype_dep, harfbuzz_dep, execinfo_dep] + ffmpeg_deps,
)

xkit_dep = declare_dependency(
  link_with           : libxkit,
  include_directories : xkit_inc,
  # xrender_dep propagates transitively to every consumer of xkit_dep;
  # execinfo_dep so consumers link backtrace() pulled in by crash_handler.
  dependencies        : [xrender_dep, thread_dep, gbm_dep, egl_dep, glesv2_dep, execinfo_dep] + ffmpeg_deps,
)

# ── Unit tests ────────────────────────────────────────────────────────────────
if gtest_dep.found()
  foreach t : [
    ['xkit_scroll_view',   'tests/unit/scroll_view_test.cc',   'xkit ScrollView'],
    ['xkit_adaptive',      'tests/unit/adaptive_test.cc',      'xkit Adaptive'],
    ['xkit_touch_target',  'tests/unit/touch_target_test.cc',  'xkit TouchTarget'],
    ['xkit_text_input',    'tests/unit/text_input_test.cc',    'xkit TextInput'],
    ['xkit_image',         'tests/unit/image_test.cc',         'xkit ImageWidget'],
    ['xkit_layout',        'tests/unit/layout_test.cc',        'xkit Layout'],
    ['xkit_animation',     'tests/unit/animation_test.cc',     'xkit Animation'],
    ['xkit_hit_test',      'tests/unit/hit_test.cc',           'xkit HitTest'],
    ['xkit_three_tree',    'tests/unit/three_tree_test.cc',    'xkit ThreeTree'],
    ['xkit_event',         'tests/unit/event_test.cc',         'xkit Event'],
    ['xkit_cursor',        'tests/unit/cursor_test.cc',        'xkit Cursor'],
    ['xkit_tab_model',     'tests/unit/tab_model_test.cc',     'xkit TabModel'],
    ['xkit_overlay',       'tests/unit/overlay_test.cc',       'xkit Overlay'],
    ['xkit_widget_render', 'tests/unit/widget_render_test.cc',  'xkit WidgetRender'],
    ['xkit_multiline',     'tests/unit/multiline_input_test.cc','xkit MultilineInput'],
    ['xkit_text_edit',     'tests/unit/text_edit_test.cc',      'xkit TextEdit'],
    ['xkit_cursor_input',  'tests/unit/cursor_input_test.cc',   'xkit CursorInput'],
    ['xkit_dropdown',      'tests/unit/dropdown_test.cc',      'xkit Dropdown'],
    ['xkit_dialog',        'tests/unit/dialog_test.cc',        'xkit Dialog'],
    ['xkit_menu',          'tests/unit/menu_test.cc',          'xkit Menu'],
    ['xkit_pointer_button','tests/unit/pointer_button_test.cc', 'xkit PointerButton'],
    ['xkit_segmented',     'tests/unit/segmented_control_test.cc','xkit SegmentedControl'],
    ['xkit_table',         'tests/unit/table_test.cc',         'xkit Table'],
    ['xkit_toast',         'tests/unit/toast_test.cc',         'xkit Toast'],
    ['xkit_expander',      'tests/unit/expander_test.cc',      'xkit Expander'],
    ['xkit_stepper',       'tests/unit/stepper_test.cc',       'xkit Stepper'],
    ['xkit_calendar',      'tests/unit/calendar_test.cc',      'xkit Calendar'],
    ['xkit_date_picker',   'tests/unit/date_picker_test.cc',   'xkit DatePicker'],
    ['xkit_spinner',       'tests/unit/spinner_test.cc',       'xkit Spinner'],
  ]
    test(t[2],
      executable(t[0], t[1],
        include_directories : xkit_inc,
        dependencies        : [gtest_dep, xkit_dep],
      ),
      suite : 'unit',
    )
  endforeach

  # Runtime test: drives the real VulkanEngine and reads pixels back. Needs a GPU
  # (skips itself when Vulkan is unavailable), so it lives in the 'runtime' suite,
  # not 'unit'. Links vulkan explicitly since it constructs the engine directly
  # (vulkan_dep comes from the parent scope, defined before subdir('sdk/xkit')).
  test('xkit EngineRender',
    executable('xkit_engine_render', 'tests/runtime/engine_render_test.cc',
      include_directories : xkit_inc,
      dependencies        : [gtest_dep, xkit_dep, vulkan_dep],
    ),
    suite : 'runtime',
    # The engine loads compiled UI shaders (ui_*.spv), which are generated into
    # <builddir>/freebsdx.p by the main target. Point the engine at them so init()
    # succeeds no matter what the build directory is called.
    env : ['FREEBSDX_SHADER_DIR=' + meson.project_build_root() / 'freebsdx.p'],
  )

  # Not a test — a manual pixel-proof tool for the dogfooded widgets
  # (Expander/Stepper/DatePicker). Renders them through the real engine and dumps
  # /tmp/dogfood.ppm. Run with FREEBSDX_SHADER_DIR pointed at the shader dir.
  executable('xkit_dogfood_render', 'tests/runtime/dogfood_render.cc',
    include_directories : xkit_inc,
    dependencies        : [xkit_dep, vulkan_dep],
  )
endif
