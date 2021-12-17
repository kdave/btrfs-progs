Resize
======

A BTRFS mounted filesystem can be resized after creation, grown or shrunk. On a
multi device filesystem the space occupied on each device can be resized
independently. Data that reside in the area that would be out of the new size
are relocated to the remaining space below the limit, so this constrains the
minimum size to which a filesystem can be shrunk.

Growing a filesystem is quick as it only needs to take note of the available
space, while shrinking a filesystem needs to relocate potentially lots of data
and this is IO intense. It is possible to shrink a filesystem in smaller steps.
