Internal APIs
=============

There's some common functionality found in many places like help, parsing
values, sorting, extensible arrays, etc. Not all places are unified and use old
code implementing it manually. Below is list of usable APIs that should be spread
and updated where it's still not. A need for new API might emerge from
cleanups, then it should appear here.

Option parsing
--------------

Files: common/help.h, common/parse-utils.h

Global options need to be processed and consumed by `clean_args_no_options`,
argument count by `check_argc_*`, `usage_*` for handling usage.

Options are parsed by `getopt` or `getopt_long`. Individual values from options
are recognized by `parse_*`, basic types and custom types are supported.

TODO
----

Undocumented or incomplete APIs:

* common/array.h
* common/cpu-utils.h
* common/device-utils.h
* common/messages.h
* common/open-utils.h
* common/path-utils.h
* common/sort-utils.h
* common/string-table.h
* common/string-table.h
* common/task-utils.h
* common/units.h
