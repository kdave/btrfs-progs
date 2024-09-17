Internal APIs
=============

There's some common functionality found in many places like help, parsing
values, sorting, extensible arrays, etc. Not all places are unified and use old
code implementing it manually. Below is list of usable APIs that should be spread
and updated where it's still not. A need for new API might emerge from
cleanups, then it should appear here. The text below gives pointers and is not
extensive, search the definitions and actual use in other code too.

Option parsing
--------------

Files: :file:`common/help.h`, :file:`common/parse-utils.h`

Global options need to be processed and consumed by `clean_args_no_options`,
argument count by `check_argc_*`, `usage_*` for handling usage.

Options are parsed by `getopt` or `getopt_long`. Individual values from options
are recognized by `parse_*`, basic types and custom types are supported.

Size unit pretty printing
-------------------------

Files: :file:`common/units.h`

Many commands print byte sizes with suffixes and the output format can be
affected by command line options. In the help text the options are specified by
either `HELPINFO_UNITS_SHORT_LONG` (both long and short options) or just
`HELPINFO_UNITS_LONG` in case the short option letters would conflict.

Automatic parsing of the options from *argv* is done by `get_unit_mode_from_arg`.
Printing options is done by `pretty_size_mode` which takes the value and option
mode. Default mode is human readable, the macros defining the modes are from
`UNITS_*` namespace.

File path handling
------------------

Files: :file:`common/path-utils.h`

The paths on Linux can be at most PATH_MAX, which is 4096 (:command:`getconf`).
For easier handling use a local variable like :code:`char path[PATH_MAX] = { 0 };`
and for concatenation helpers :code:`path_cat_out()` or
:code:`path_cat_out3()` and check the error values for overflows. There are
helpers to check file type :code:`path_is_*()`.

TODO
----

Undocumented or incomplete APIs:

* common/array.h
* common/cpu-utils.h
* common/device-utils.h
* common/messages.h
* common/open-utils.h
* common/sort-utils.h
* common/string-table.h
* common/task-utils.h
