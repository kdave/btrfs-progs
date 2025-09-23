Development notes
=================

Collection of various notes about development practices, how-to's or
checklists.

Adding a new ioctl, extending an existing one
---------------------------------------------

-  add code to `strace <https://github.com/strace/strace>`__ so the ioctl calls
   are parsed into a human readable form. Most of the ioctls are already
   `implemented <https://github.com/strace/strace/blob/master/src/btrfs.c>`__ and
   can be used a reference.

Tracepoints
-----------

The tracepoint message format should be compact and consistent, so please stick
to the following format:

-  *key=value* no spaces around *=*
-  separated by spaces, not commas
-  named values: print value and string, like "%llu(%s)", no space between,
   string in parens
-  avoid abbreviating key values too much, (e.g. use 'size' not 'sz')
-  hexadecimal values are always preceded by *0x* (use "0x%llx")
-  use *struct btrfs_inode* for inode types, not plain *struct inode*
-  inode number type is *u64*, use *btrfs_ino* if needed
-  key can be printed as *[%llu,%u,%llu]*
-  enum types need to be exported as *TRACE_DEFINE_ENUM*

**Example:**

event: *btrfs__chunk*

string: ``"root=%llu(%s) offset=%llu size=%llu num_stripes=%d sub_stripes=%d type=%s"``


Error messages, verbosity
-------------------------

-  use *btrfs_\** helpers (btrfs_err, btrfs_info, ...), they print a filesystem
   identification like ``BTRFS info (device sdb): ...``
-  first letter in the string is lowercase
-  message contents

   -  be descriptive
   -  keep the text length reasonable (fits one line without wrapping)
   -  no typos
   -  print values that refer to what happened (inode number, subvolume
      id, path, offset, ...)
   -  print error value from the last call
   -  no *"\\n"* at the end of the string
   -  no *".*'' at the end of text
   -  un-indent the string so it fits under 80 columns
   -  don't split long strings, overflow 80 is ok in this case (we want
      to be able to search for the error messages in the sources easily)

- value representation

   -  decimal: offsets, length, ordinary numbers
   -  hexadecimal: checksums
   -  hexadecimal + string: bitmasks (e.g. raid profiles, flags)
   -  intervals of integers:

      -  closed interval (end values inclusive): [0, 4096]
      -  half-open (right value excluded): [0, 4096)
      -  half-open (left value excluded): (0, 4096] -- that one may look
         strange but is used in several places

Message level
-------------

-  btrfs_err -- such messages have high visibility so use them for serious
   problems that need user attention
-  btrfs_warn -- conditions that are not too serious but can point to potential
   problems, the system should be still in a good state
-  btrfs_info -- use for informative messages that are useful to see what's
   happening in the filesystem or might help debugging problems in the future
   and are worth keeping in the logs

Error handling and transaction abort
------------------------------------

Please keep all transaction abort exactly at the place where they happen and do
not merge them to one. This pattern should be used everywhere and is important
when debugging because we can pinpoint the line in the code from the syslog
message and do not have to guess which way it got to the merged call.

Error handling and return values
--------------------------------

Functions are supposed to handle all errors of the callees and clean up the
local context before returning. If a function does not need to pass errors to
the caller it can be switched to return *void*. Before doing so make sure that:

-  the function does not call any BUG/BUG_ON
-  all callees properly handle errors and do not call BUG/BUG_ON in place of
   error handling
-  the whole call chain starting from the function satisfies the above

Handling unexpected conditions
------------------------------

This is different than error handling. An unexpected condition happens when the
code invariants/assumptions do not hold and there's no way to recover from the
situation. This means that returning an error to the caller can't be done and
continuing would only propagate the logic error further. The reasons for that
bug can be two fold: internal (a genuine bug) or external (e.g. memory bitflip,
memory corrupted by other subsystem). In this case it is allowed to use the
nuclear option and do BUG_ON, that is otherwise highly discouraged.

There are several ways how to react to the unexpected conditions:

-  btrfs_abort_transaction()

   The recommended way if and only if we can not recover from the situation and
   have a transaction handle.

   This would cause the filesystem to be flipped read-only to prevent further
   corruption.

   Additionally call trace would be dumped for the first btrfs_abort_transaction()
   call site.

-  ASSERT()

   Conditionally compiled in and crashes when the condition is false.

   This should only be utilized for debugging purposes, acts as a fail-fast
   option for developers, thus should not be utilized for error handling.

   It's recommended only for very basic (thus sometimes unnecessary) requirements.
   Such usage should be easy to locate, have no complex call chain.
   E.g. to rule out invalid function parameter combinations.

   Should not be utilized on any data/metadata reads from disks, as they can be
   invalid. For sanity check examples of on-disk metadata, please refer to
   `Tree checker`.

-  WARN_ON

   Unconditional and noisy checks, but still allow the code to continue.

   Should only be utilized if a call trace is critical for debugging.

   Not recommended if:

   *  The call site is unique or can be easily located

      In that case, an error message is recommended.

   *  The call site would eventually lead to a btrfs_abort_transaction() call

      btrfs_abort_transaction() call would dump the stack anyway.
      If the call trace is critical, it's recommended to move the
      btrfs_abort_transaction() call closer to the place where the error happens.

-  BUG_ON

   Should not be utilized, and is incrementally removed or replaced in the code.

Error injection using eBPF
--------------------------

Functions can be annotated to enable error injection using the eBPF scripts.
See e.g. ``disk-io.c:open_ctree``. For btrfs-specific injection, the annotation
is ALLOW_ERROR_INJECTION, but beware that this only overrides the return value
and this can leak memory or other resources.  For error injection to generic
helpers (e.g. memory allocation), you can use something like
``bcc/tools/inject.py kmalloc btrfs_alloc_device() -P 0.001``

Resources:

-  `eBPF <https://ebpf.io/>`_
-  `BCC tools <https://github.com/iovisor/bcc>`_

Warnings and issues found by static checkers and similar tools
--------------------------------------------------------------

There are tools to automatically identify known issues in code and report them
as problems to be fixed, but not all such reports are valid or relevant in the
context of the code base. The fix should really fix the code, not just the
tool's warning. Such patches will be rejected with explanation first time and
ignored when sent repeatedly. Patches fixing real problems with a good
explanation are welcome. If you're not sure about sending such patch, please
ask the https://kernelnewbies.org/KernelJanitors for help.

Do not blindly report issues caught by:

-  checkpatch.pl -- the script is good for catching some coding style but this
   whole wiki page exists to be explicit what we want, not necessarily what
   checkpatch wants
-  clang static analyzer -- lots of the reports are not real problems and may
   depend on a condition that's not recognized by the checker
-  gcc -Wunused -- any of the -Wunused-\* options can report a valid issue but
   it must be viewed in wider context and not just removed to get rid of the
   warning
-  codespell -- fixing typos is fine but should be done in batches and over
   whole code base

Hints:

-  if you find an issue, look in the whole code base if there are more instances
   same or following a similar pattern
-  look into git history of the changed code, why it got there and when, it may
   help to understand if it's a bug or e.g. a stale code

Coding style preferences
------------------------

Before applying recommendations from this sections, please make sure you're
familiar with the `kernel coding style guide
<https://www.kernel.org/doc/html/latest/process/coding-style.html%7Cgeneric>`__.

The purpose of coding style is to maintain unified and consistent look & feel
of the patches and code, keeping distractions to minimum which decreases
cognitive load and allows focus on the important things.  Coding style is not
only where to put white space or curly brackets but also coding patterns with
meaning that is established and understood in the developer group. The code in
linux kernel is maintained for a long period of time and maintainability is of
crucial importance. This means it does take time to write good code, with
attention to detail. Once written the code could stay unchanged for years but
will be read many times. `Read more
<https://simpleprogrammer.com/maintaining-code/>`__.

General advice: *Try to keep the same style and formatting of the code that's
already there.*

Patches
^^^^^^^

-  for patch subject use "btrfs:" followed by a lowercase
-  read the patch again and fix all typos and grammar
-  size units should use short form K/M/G/T or IEC form KiB/MiB/GiB
-  don't write references to parameters to subject (like removing @pointer)
-  do not end subject with a dot '.'
-  parts of btrfs that could have a subject prefix to point to a specific subsystem

    -  scrub, tests, integrity-checker, tree-checker, discard, locking, sysfs,
       raid56, qgroup, compression, send, ioctl

-  additional information

    -  if there's a stack trace relevant for the patch, add it there (lockdep,
       crash, warning)
    -  steps to reproduce a bug (that will also get turned to a proper fstests
       case)
    -  sample output before/after if it could have impact on userspace
    -  `pahole <https://linux.die.net/man/1/pahole>`_ output if structure is being reorganized and optimized

Function declarations
^^^^^^^^^^^^^^^^^^^^^

-  avoid prototypes for static functions, order them in new code in a way that
   does not need it

   -  but don't move static functions just to get rid of the prototype

-  exported functions have btrfs\_ prefix
-  do not use functions with double underscore, there's only a few valid uses of
   that, namely when *\__function* is doing the core of the work with looser
   checks, no locks or more parameters than *function*
-  function type and name are on the same line, if this is too long, the
   parameters continue on the next line (indented)
-  'static inline' functions should be small (measured by their resulting binary
   size)
-  conditional definitions should follow the style below, where the full
   function body is in .c

.. code-block:: c

   #ifdef CONFIG_BTRFS_DEBUG
   void btrfs_assert_everything_is_fine(void *ptr);
   #else
   void btrfs_assert_everything_is_fine(void *ptr) { }
   #endif

Headers
^^^^^^^

-  leave one newline before #endif in headers
-  include headers that add usage of a data structure or API, also remove such
   header with last use of the API

Comments
^^^^^^^^

-  function comment goes to the .c file, not the header

   -  kdoc style is recommended but the exact syntax is not mandated and
      we're using only subset of the formatting
   -  the first line (mandatory): contains a brief description of what
      the function does and should provide a summary information

      -  do write in the imperative style "Iterate all pages and clear
         some bits"
      -  don't write "This function is a helper to ...", "This is used
         to ..."

   -  parameter description (optional):

      -  each line describes the parameter
      -  the list needs to be in the same order as for the function
      -  the list needs to be complete
      -  trivial parameters don't need to be explained, e.g. fs_info is
         clear so the description could be 'the filesystem'
      -  context of the parameters matters a lot in some cases and
         cannot be inferred from the name, then it should be documented

.. code-block:: c

   /*
    * Look for blocks in the given offset.
    * 
    * @fs_info:    trivial parameters should be in the list but with some short description
    * @offset:     describe the context of the argument, e.g. offset to page or inode ...
    *
    * Long description comes here if necessary.
    *
    * Return value semantics if it's not obvious
    */

-  comment new enum/define values, brief description or pointers to the code
   that uses them
-  comment new data structures, their purpose and context of use
-  do not put struct member comments on the same line, put it on the line
   before and do not trim/abbreviate the text
-  comment text that fits on one line can use the */\* text \*/* format, slight
   overflow of 80 chars is OK

Misc
^^^^

-  fix spelling, grammar and formatting of comments that get moved or changed
-  fix coding style in code that's only moved
-  one newline between functions
-  80 chars per line are recommended but longer lines are OK (up to 90) if the
   code "looks better" without the line break, e.g. if half of the word is beyond 80 chars
   but it's clear what it is, or function prototypes do not need to wrap arguments

Locking
^^^^^^^

-  do not use ``spin_is_locked`` but ``lockdep_assert_held``
-  do not use ``assert_spin_locked`` without reading it's semantics (it does
   not check if the caller hold the lock)
-  use ``lockdep_assert_held`` and its friends for lock assertions
-  add lock assertions to functions called deep in the call chain

Code
^^^^

-  default function return value is *int ret*, temporary return values should
   be named like *ret2* etc
-  structure initializers should use *{ 0 }*
-  do not use *short int* type if possible, if it fits to char/u8 use it instead,
   or plain int/u32
-  memory barriers need to be always documented
-  add *const* to parameters that are not modified
-  use bool for indicators and bool status variables (not int)
-  use matching int types (size, signedness), with exceptions
-  use ENUM_BIT for enumerated bit values (that don't have assigned fixed numbers)
-  add function annotations __cold, __init, __attribute_const__ if applicable
-  use automatic variable cleanup for:
   -  *struct btrfs_path* with BTRFS_PATH_AUTO_FREE
-  use of ``unlikely()`` annotation is OK and recommended for the following cases:

   -  control flow of the function is changed due to error handling and it
      leads to *never-happens* errors like EUCLEAN, EIO

Output
^^^^^^

-  when dumping a lot of data after an error, consider what will remain visible
   last

   -  in case of ``btrfs_print_leaf``, print the specific error message after
      that

Expressions, operators
^^^^^^^^^^^^^^^^^^^^^^

-  spaces around binary operators, no spaces for unary operators
-  extra braces around expressions that might be harder to understand wrt
   precedence are fine (logical and/or, shifts with other operations)

   -  *a \* b + c*, *(a << b) + c*, *(a % b) + c*

-  64bit division is not allowed, either avoid it completely, or use bit
   shifts or use div_u64 helpers; do not use *do_div* for division as it's a
   macro and has side effects
-  do not use chained assignments: no *a = b = c;*

Variable declarations, parameters
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

-  declaration block in a function should do only simple initializations
   (pointers, constants); nothing that would require error handling or has
   non-trivial side effects
-  use *const* extensively
-  add temporary variable to store a value if it's used multiple times in the
   function, or if reading the value needs to chase a long pointer chain
-  do not mix declarations and statements (although kernel uses C standard that
   allows that)

Kernel config options
---------------------

Compile-time config options for kernel that can help debugging, testing.  They
usually take a hit on performance or resources (memory) so they should be
selected wisely. The options in **bold** should be safe to use by default for
debugging builds.

Please refer to the option documentation for further details.

-  devices for testing

   -  **CONFIG_BLK_DEV_LOOP** - enable loop device
   -  for fstests: **DM_FLAKEY**, **CONFIG_FAIL_MAKE_REQUEST**
   -  **CONFIG_SCSI_DEBUG** - fake scsi block device

-  memory

   -  **CONFIG_SLUB_DEBUG** - boot with slub_debug
   -  CONFIG_DEBUG_PAGEALLOC + CONFIG_DEBUG_PAGEALLOC_ENABLE_DEFAULT (on
      newer kernels)
   -  CONFIG_SCHED_STACK_END_CHECK
   -  CONFIG_PAGE_POISONING
   -  CONFIG_HAVE_DEBUG_KMEMLEAK
   -  CONFIG_FAILSLAB -- fault injection to kmalloc
   -  CONFIG_DEBUG_LIST
   -  CONFIG_BUG_ON_DATA_CORRUPTION

-  btrfs

   -  **CONFIG_BTRFS_DEBUG**
   -  **CONFIG_BTRFS_ASSERT**
   -  **CONFIG_BTRFS_EXPERIMENTAL**
   -  **CONFIG_BTRFS_FS_RUN_SANITY_TESTS** -- basic tests on module load
   -  **CONFIG_BTRFS_FS_CHECK_INTEGRITY** -- block integrity checker
      enabled by mount options
   -  **CONFIG_BTRFS_FS_REF_VERIFY** -- additional checks for block
      references

-  locking

   -  CONFIG_DEBUG_SPINLOCK, CONFIG_DEBUG_MUTEXES
   -  CONFIG_DEBUG_LOCK_ALLOC
   -  CONFIG_PROVE_LOCKING, CONFIG_LOCKDEP
   -  CONFIG_LOCK_STAT
   -  CONFIG_PROVE_RCU
   -  CONFIG_DEBUG_ATOMIC_SLEEP

-  sanity checks

   -  CONFIG_DEBUG_STACK_USAGE, CONFIG_HAVE_DEBUG_STACKOVERFLOW,
      CONFIG_DEBUG_STACKOVERFLOW
   -  CONFIG_STACKTRACE
   -  CONFIG_KASAN -- address sanitizer
   -  CONFIG_UBSAN -- undefined behaviour sanitizer
   -  CONFIG_KCSAN -- concurrency checker

-  verbose reporting

   -  CONFIG_DEBUG_BUGVERBOSE

-  tracing

   -  CONFIG_TRACING etc

BUG: MAX_LOCKDEP_CHAIN_HLOCKS too low!
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Not a bug and please don't report it. The lockdep structures can get in some
cases full and cannot properly track locks anymore. There's only a workaround
to increase the kernel config value of CONFIG_LOCKDEP_CHAINS_BITS, default is
16, 18 tends to work, increase if needed.

fstests setup
-------------

The `fstests <https://git.kernel.org/pub/scm/fs/xfs/xfstests-dev.git/>`_
suite has very few "hard" requirements and will succeed without
actually running many tests. In order to ensure full test coverage, your test
environment should provide the settings from the following sections. Please
note that newly added tests silently add new dependencies, so you should always
review results after an update.


Kernel config options for complete test coverage
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

-  ``CONFIG_FAULT_INJECTION=y``
-  ``CONFIG_FAULT_INJECTION_DEBUG_FS=y``
-  ``CONFIG_FAIL_MAKE_REQUEST=y``
-  ``CONFIG_DM_FLAKEY=m`` or ``y``
-  ``CONFIG_DM_THIN_PROVISIONING=m`` or ``y``
-  ``CONFIG_DM_SNAPSHOT=m`` or ``y``
-  ``CONFIG_DM_DELAY=m`` or ``y``
-  ``CONFIG_DM_ERROR=m`` or ``y``
-  ``CONFIG_DM_LOG_WRITES=m`` or ``y``
-  ``CONFIG_DM_DUST=m`` or ``y``
-  ``CONFIG_DM_ZERO=m`` or ``y``
-  ``CONFIG_BLK_DEV_LOOP=m`` or ``y``
-  ``CONFIG_EXT4_FS=m`` or ``y``
-  ``CONFIG_SCSI_DEBUG=m``
-  ``CONFIG_BLK_DEV_ZONED=y`` for zoned mode test coverage
-  ``CONFIG_IO_URING==y``


Kernel config options for better bug reports
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

See the list in the section above for more options.


User space utilities and development library dependencies
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

-  acl
-  attr
-  btrfsprogs
-  dbench
-  dmsetup (device-mapper)
-  duperemove
-  e2fsprogs
-  fio
-  fsverity-utils
-  libacl
-  libaio
-  libattr
-  libcap-progs
-  libuuid
-  lvm2
-  openssl
-  parted
-  psmisc (killall)
-  xfsprogs >= 4.3.1 (``xfs_io -c reflink`` is required)

Note: This list may be incomplete.

Storage environment
^^^^^^^^^^^^^^^^^^^

-  at least 4 identically sized partitions/disks/virtual disks, specified using
   ``$SCRATCH_DEV_POOL``
-  some tests may require 8 equally sized``SCRATCH_DEV_POOL`` partitions
-  some tests need at least 10G of free space, as determined by ``df``, i.e.
   the size of the device may need to be larger, 12G should work
-  some tests require ``$LOGWRITES_DEV``, yet another separate block device,
   for power fail testing
-  for testing trim and discard, the devices must be capable of that (physical
   or virtual)

Other requirements
^^^^^^^^^^^^^^^^^^

-  An ``fsgqa`` user and group must exist.
-  An ``fsgqa2`` user and group must exist.
-  The user ``nobody`` must exist.
-  An ``123456-fsgqa`` user and group must exist.
