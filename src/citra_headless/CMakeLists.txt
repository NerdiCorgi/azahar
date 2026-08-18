add_executable(citra_headless
    main.cpp
    headless_window.cpp
)

target_link_libraries(citra_headless PRIVATE citra_common citra_core input_common)
