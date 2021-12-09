Deduplication
=============

Going by the definition in the context of filesystems, it's a process of
looking up identical data blocks tracked separately and creating a shared
logical link while removing one of the copies of the data blocks. This leads to
data space savings while it increases metadata consumption.

There are two main deduplication types:

* **in-band** *(sometimes also called on-line)* -- all newly written data are
  considered for deduplication before writing
* **out-of-band** *(sometimes alco called offline)* -- data for deduplication
  have to be actively looked for and deduplicated by the user application

Both have their pros and cons. BTRFS implements **only out-of-band** type.

BTRFS provides the basic building blocks for deduplication allowing other tools
to choose the strategy and scope of the deduplication.  There are multiple
tools that take different approaches to deduplication, offer additional
features or make trade-offs. The following table lists tools that are known to
be up-to-date, maintained and widely used.

.. list-table::
   :header-rows: 1

   * - Name
     - File based
     - Block based
     - Incremental
   * - `BEES <https://github.com/Zygo/bees>`_
     - No
     - Yes
     - Yes
   * - `duperemove <https://github.com/markfasheh/duperemove>`_
     - Yes
     - No
     - Yes

Legend:

- *File based*: the tool takes a list of files and deduplicates blocks only from that set
- *Block based*: the tool enumerates blocks and looks for duplicates
- *Incremental*: repeated runs of the tool utilizes information gathered from previous runs
