function(companion_enable_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4 /WX /permissive- /EHsc /utf-8
      /wd4251
    )
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Werror
    )
  endif()
endfunction()
