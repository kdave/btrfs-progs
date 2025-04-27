JSON output
===========

Supported types:

* any: a valid *printf* format, parameters to *fmt_print()* must match the number (and are not validated)
* number: ``%llu``
* string: ``%s``
* string: ``str`` - backslash escaped special characters (0x08, 0x08, 0x0a, 0x0c, 0x0d, 0x0b),
  the rest of range from *0x00* to *0x1f* as *\\uXXXX* encoding
* bool: ``bool`` - unquoted native json values *true* or *false*
* qgroupid: ``qgroupid`` - split to 48/16 for level/subvolid
* size: ``size`` - size with SI/IEC size suffix
* size: ``size-or-none`` - same as *size* but for 0 it's *none*
* UUID: ``uuid`` - if all zeros then *null* (native json), or properly formatted UUID string
* date + time: ``date-time`` - timestamp formatted as *YYYY-MM-DD HH:MM:SS TIMEZONE*
* duration: ``duration`` - difference of two timestamps, like *D days HH:MM:SS* (days not show of 0)

Commands that support json output
---------------------------------

* :command:`btrfs device stats`
* :command:`btrfs filesystem df`
* :command:`btrfs filesystem show`
* :command:`btrfs qgroup show`
* :command:`btrfs subvolume get-default`
* :command:`btrfs subvolume list`
* :command:`btrfs subvolume show`

Recommendations
---------------

Keys and formatting are defined as an array of *struct rowspec*.

* key names
   * should be unified with the printed value if they mean the same thing
   * not abbreviated (e.g. *generation* instead of *gen*)
   * referring to existing and well known names (qgroupid, devid, ...)
   * using ``-`` as word separator, or ``_`` if it's better to keep the same name of the value
* values
   * numbers without suffix or other transformation, i.e. no *KiB*
   * formatted by the types if possible
   * any *printf* format is possible but should be avoided or a new type should
     be defined
* printing more data about an item is better than printing less, assuming the
  filtering is done on the user side
* structure of json output may not reflect the way it's printed in plain text,
  in that case do two separate printer functions
* if plain and json output roughly follow the same style, e.g. line oriented
  that is easy to transform to a map, then both outputs should use the same
  rowspec
* one value can be printed by multiple rowspecs that may do different
  formatting depending on the context
