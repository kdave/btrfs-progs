Quick start
===========

For a really quick start you can simply create and mount the filesystem. Make
sure that the block device you'd like to use is suitable so you don't overwrite existing data.

.. code-block:: shell

   # mkfs.btrfs /dev/sdx
   # mount /dev/sdx /mnt/test

The default options should be acceptable for most users and sometimes can be
changed later. The example above is for a single device filesystem, creating a
*single* profile for data (no redundant copies of the blocks), and *DUP*
for metadata (each block is duplicated).
