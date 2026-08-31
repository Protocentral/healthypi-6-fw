zephyr_library()

zephyr_library_sources(
  afe4400.c
  afe4400_async.c
)

# Export the driver dir globally (like ads129xx) so app code can include the
# public afe4400.h header, not just the driver's own sources.
zephyr_include_directories(.)
