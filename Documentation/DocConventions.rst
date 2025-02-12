Conventions and style for documentation
---------------------------------------

Manual pages structure:

-  add references to all external commands mentioned anywhere in the text to the *SEE ALSO* section
-  add related, not explicitly mentioned commands or pages
-  the heading levels are validated, underlined text by the following

   -  mandatory, manual page ``===``
   -  mandatory, sections ``---``
   -  optional, sub-sections ``^^^``
   -  optional, paragraphs ``"""``

-  command-specific examples are mostly free of restrictions but should be
   readable in all output formats (manual page, html)

-  subcommands are in alphabetical order

-  long command output or shell examples: verbatim output
   -  use ``..code-block::`` directive with ``bash`` or ``plain`` syntax highlighting

Quotes, reference, element formatting:

-  exact syntax: monotype ````usage=0````
-  reference to arguments: italics ``*italics*``
-  command reference:

   -  any system command, example, bold text by directive ``:command:`btrfs filesystem show```
   -  subcommands with their own manual page ``:doc:`btrfs-filesystem```
   -  subcommand names should be spelled in full, i.e. *filesystem* instead of *fi*

-  file, directory or path references: by directive ``:file:`/path```

-  section references without a label: italics ``*EXAMPLES*``
-  section references with a target label: reference by directive ``:ref:`visible text <target-label>```

-  argument name in option description: caps in angle brackets ``<NAME>``

   -  reference in help text: caps ``NAME``
   -  also possible: caps italics ``*NAME*``

-  command short description:

   -  command name: bold (not by directive) ``**command**``
   -  optional unspecified: brackets ``[options]``
   -  mandatory argument: angle brackets ``<path>``
   -  optional parameter with argument: ``[-p <path>]``


Referencing:

-  add target labels for commands that are referenced and replace command name
   with the reference target

-  NOTE: we have either full doc reference by ``:doc:`docname``` or
   inter-document reference to an **unambiguous** label
   ``:ref:`target-label```, i.e. this can't reference a label that may appear in
   more files due to including, this will lead to the document itself that may
   not be the full page

-  **ambiguous** or duplicate labels (that exist in a file that is included from other documents)
   need to be

   -  defined as ``.. duplabel:: labelname``
   -  referenced as ``:docref:`visible text <document:label>```

-  generic links can use the free form link syntax with description ```Link text <https://example.com>`__``
   (note the double underscore, this is *anonymous* link and does not create a reference)
   or plain link that will auto-render to a clickable link https://example.com

-  in manual pages: always use full link as it's meant to be read in terminal
   output and must allow copy&paste

-  own manual page references:

   - ``:doc:`btrfs-filesystem```, i.e. it's the document name, it will render the section automatically
   - other manual pages ``:manref:page(1)``, the exact name and section number

-  add more clickable references rather than less

-  custom rules and directives are implemented in :file:`Documentation/conf.py`

Conventions:

-  version should be formatted like ``6.1`` or ``v6.1`` and clear what
   project/tool it's related to unless it's obvious from the context


Other:

-  for notes use ``.. note::`` directive, is rendered as a separate paragraph and
   should be used only for important information

-  ``.. warning::`` directive is rendered as a separate paragraph
   and most likely more visible than NOTE, use for critical information that
   may cause harm, irreversible state or performance problems

   -  should point reader to other part of documentation to seek more details


References:

-  RST https://www.sphinx-doc.org/en/master/
-  RST and Sphinx Cheatsheet https://spl.hevs.io/spl-docs/writing/rst/cheatsheet.html
-  RST Cheat Sheet https://sphinx-tutorial.readthedocs.io/cheatsheet/
