JSON output
===========

Supported types:

* number: ``%llu``
* string: ``%s``
* string: ``str`` (escaped special characters)
* bool: ``bool`` (unquoted native json value)
* qgroupid: ``qgroupid`` (split to 48/16 for level/subvolid)
* size: ``size``

Commands that support json output
---------------------------------

* :command:`btrfs device stats`
* :command:`btrfs filesystem df`
* :command:`btrfs qgroup show`
* :command:`btrfs subvolume get-default`
* :command:`btrfs subvolume list`
* :command:`btrfs subvolume show`

Recommendations
---------------

* key names
   * should be unified if they mean the same thing
   * not abbreviated (e.g. *generation* instead of *gen*)
   * referring to existing and well known names (qgroupid, devid, ...)
* values
   * numbers without suffix or other transformation, i.e. no *KiB*
* printing more data about an item is better than printing less, assuming the
  filtering is done on the user side
* structure of json output may not reflect the way it's printed in plain text,
  in that case do two separate printer functions
* if plain and json output roughly follow the same style, e.g. line oriented
  that is easy to transform to a map, then both outputs should use the same
  rowspec
