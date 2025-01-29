Command line, formatting, UI guidelines
=======================================

## Command line options

### Short options

* reserved for the most common operations
* should follow some common scheme
 * verbose, recursive, force, output redirection, ...
 * same option means the same thing in a group of related options
* most have an equivalent long option

### Long options

* short but descriptive
* long-worded long options are acceptable for rare but seemingly unique operations
  * example: `btrfs check --clear-space-cache v1`

### Help text

* short help for *--help* output: `btrfs subcommand [options] param1 param2`
 * the number of options gets unwieldy for many options so it's better to
   insert the stub and document properly all of them in the detailed section
* short description after command line: terse but understandable explanation
  what the command does
* long description after the short description
 * explain in more detail what the command does, different use cases, things to notice
 * more complex things should be documented in the manual pages and pointed to

## Command output, verbosity

### Structured output

* `Key: value`
* indentation used for visual separation
* value column alignment for quick skimming
* must be parseable by scripts but primary consumer of the output is a human, and greppable for logs

### Default output

* unix commands do one thing and say nothing, we may diverge a bit from that
* default output is one line shortly describing the action
 * why: running commands from scripts or among many other commands should be
   noticeable due to progress tracking or for analysis if something goes wrong
 * there's a global option to make the commands silent `btrfs -q subcommand`,
   this can be easily scripted e.g. storing the global verbosity option in a
   variable, `btrfs $quiet subcommand` and then enabling or disabling as needed
* line length should not exceed 80 columns if possible but this is not a strict
  limit and we want to put the relevant information there rather abbreviate too
  much

### Formatting

* numeric values are not quoted
* string values are quoted if they would be confused when parsed word by word
  (e.g. file paths)
  * quoting is by single apostrophe on both ends
* all messages follow a few known and understood schemes

### Verbosity levels

* 0 - quiet, only errors to stderr, nothing to stdout
* 1 - default, one line, see above
* 2 - inform about main steps that happen
* 3 - a little bit more details about what happens during level 2
* 4 - lots of information, for debugging and close inspection what happens
