Command line, formatting, UI guidelines
=======================================

The guidelines try to follow common principles and build on experience based on
user feedback or practices seen in other projects. There are no strict rules
but rather vague statements or principles that is recommended to follow.
It's recommended to follow them or use them as review checklist. Optimize for
humans.

- *sane defaults*
- *principle of least surprise*
- *it does what it says*
- *it says what it does*
- *frequently performed actions have shortcuts*
- *easy thing easy, hard things possible*
- *dangerous actions are explicit or need a confirmation*
- *same name means the same thing everywhere*
- *it's hard to change things once they are released*

Command line options
--------------------

Unless there's a precedent for using a well known short option name, using
long option for first implementation is always safe.

All options parsers use :manref:`getopt(3)`, the behaviour is known and
consistent with most tools and utilities found elsewhere. Options and parameters
are sorted (options first) up to the ``--`` delimiter. Global options follow right
after the main command and are parsed separately from the subcommand, :command:`btrfs`,
following syntax
:command:`btrfs [global options] subcommand [options] <parameters...>`.

Short options
^^^^^^^^^^^^^

Short options are in short supply, ``a-z`` and ``A-Z``.

*  reserved for the most common operations
*  should follow some common scheme

   * verbose (-v), recursive (-r, -R), force (-f), output redirection (-o), ...
   * same option means the same thing in a group of related options
   * mnemonic naming when possible, e.g. first letter of the action like
     *-l* for *length* but with growing number of features clashes can and will
     happen

*  *upper case* could mean negating or extending meaning of lower case if it
   exists, using both upper and lower case for different purposes can be
   confusing
*  most short options should have an equivalent long option
*  rarely done actions do not need short options at all

Long options
^^^^^^^^^^^^

Long options are meant to be descriptive, e.g. when used in scripts, documentation
or change descriptions.

*  brief but descriptive
*  long and descriptive can be used in justified cases (e.g. conversion options
   in :doc:`btrfstune` because of the single command without subcommands)

Option parameters
^^^^^^^^^^^^^^^^^

.. note::
   **Avoid using optional parameter.** This is a usability misfeature of
   *getopt()* because of the mandatory short option syntax where the parameter
   *must* be glued to the option name, like *-czstd* so this looks like a group
   of short options but it is not. In this example *-c zstd* is not the same,
   the parameter will take default value and *zstd* will be understood as
   another parameter. Unfortunate examples are :command:`btrfs filesystem
   defrag -c` and :command:`btrfs balance start -d`. Both quite common and we
   probably cannot fix this without breaking existing scripts.

Help text
^^^^^^^^^

Description in the help text should be long enough to describe what it does, mention default
value and should not be too long or detailed. Referring to documentation is recommended
when it's really wise to read it first before using it. Otherwise it's a reminder
to user who has probably used it in the past.

*  short help for *--help* output: :command:`btrfs subcommand [options] param1 param2`

   * the number of options gets unwieldy for a command with many tunable features
     so it's better to write a short description and document properly everything
     in the manual page or in the followup text

*  short description after command line: terse but understandable explanation
   what the command does, mention dangers

*  long description after the short description

   *  explain in more detail what the command does, different use cases, things to notice
   *  more complex things should be documented in the manual pages and pointed to
      (examples)

Command output, verbosity
-------------------------

Structured output
^^^^^^^^^^^^^^^^^

If the output consists of a lot of information, try to present it in a concise
way and structure related information together using some known formats
or already used ones in this project.

* `Key: value`, spacing by tabs or spaces
* use indentation and empty lines for visual separation
* value column alignment for quick skimming
* must be parseable and also visually comprehensible, related information
  on one line usually satisfies both (*greppable*)

Default output
^^^^^^^^^^^^^^

* UNIX commands do one thing and say nothing, we diverge from that as it does
  not work well for a multi-command tools
* default output is one line shortly describing the action

  * why: running commands from scripts or among many other commands should be
    noticeable due to progress tracking or for analysis if something goes wrong
  * there's a global option to make the commands silent :command:`btrfs -q subcommand`,
    this can be easily scripted e.g. storing the global verbosity option in a
    variable, :command:`btrfs $quiet subcommand` and then enabling or disabling as needed

* there should be a line length limit so the output visually fits to one line without
  wrapping, there's no exact number of columns, assume something around 100,
  keep related information on one line (printed) rather then breaking it.

Formatting
^^^^^^^^^^

* numeric values are not quoted
* string values are quoted if they would be confused when parsed word by word
  (e.g. file paths)
  * quoting is by single apostrophe on both ends, no fancy backtick+quote
* all messages follow a few known and understood schemes

Verbosity levels
^^^^^^^^^^^^^^^^

* 0 - quiet, only errors to *stderr*, nothing to *stdout*
* 1 - default, one line, see above
* 2 - inform about main steps that happen
* 3 - a little bit more detailed about what happens during level 2
* 4 - possibly lots of information, for debugging and close inspection what happens
