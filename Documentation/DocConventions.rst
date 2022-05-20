Conventions and style for documentation
---------------------------------------

Manual pages structure:

- add references to all external commands mentioned anywhere in the text to the
  *SEE ALSO* section
  - also add related, not explicitly mentioned
- the heading levels are validated
  - mandatory, manual page ===
  - mandatory, sections ---
  - optional, sub-sections ^^^
- command-specific examples are mostly free of restrictions but should be
  readable in all output formats (manual page, html)

- subcommands are in alphabetical order

- long command output or shell examples: verbatim output
  - use code-block:: directive

Quotation in subcommands:

- exact syntax: monotype ``usage=0``
- reference to arguments etc: *italics*
- command reference: bold ``btrfs filesystem show``
  - subcommand names should be spelled in full, ie. *filesystem* instead of *fi*
- section references: italics *EXAMPLES*

- argument name in option description: caps in angle brackets <NAME>
  - reference in help text: caps NAME
    also possible: caps italics *NAME*

- command short description:
  - command name: bold **command**
  - optional unspecified: brackets [options]
  - mandatory argument: angle brackets <path>
  - optional parameter with argument: [-p <path>]

Other:

- for notes use note:: directive, is rendered as a separate paragraph and
  should be used only for important information

- warning:: directive is rendered as a separate paragraph
  and most likely more visible than NOTE, use for critical information that
  may cause harm, irreversible state or performance problems
  - should point reader to other part of documentation to seek more details

References:
- RST and Sphinx Cheatsheet https://spl.hevs.io/spl-docs/writing/rst/cheatsheet.html
- RST Cheat Sheet https://sphinx-tutorial.readthedocs.io/cheatsheet/
