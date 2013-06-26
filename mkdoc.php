#!/usr/bin/env php
<?php
// Extract markdown docs from header files
// vim:ts=2:sw=2:et:

// Which files to process
$files = array_flip(explode("\n", trim(
  stream_get_contents(
    popen('find include/phenom -name \*.h', 'r')
  )
)));

// Which files to exclude
$suppress = array_flip(array(
  'include/phenom/config.h',
  'include/phenom/defs.h',
  'include/phenom/queue.h',
));

// The final file list
$files = array_diff_key($files, $suppress);

if (!is_dir('docs')) {
  mkdir('docs');
}
foreach ($files as $incname => $_) {
  process_include($incname);
}

exit(0);

function process_include($incname) {
  $incfile = file_get_contents($incname);

  $target = preg_replace(
    ',^include/phenom/(.*)\.h$,',
    'docs/\\1.markdown',
    $incname);

  $out = fopen($target, 'w');

  preg_match_all(',/\*\*.*?\*/,s', $incfile, $matches, PREG_OFFSET_CAPTURE);
  foreach ($matches[0] as $i => $entry) {
    list($comment, $offset) = $entry;

    // Look ahead and see if we can find the first C-ish declaration.
    $decl = '';
    $title = '';

    if (preg_match(',[^/]+;,s', $incfile, $declm,
        0, $offset + strlen($comment))) {
      $decl = is_plausible_decl($declm[0], $title);
    }
    $extracted = extract_from_comment($comment, $title);

    // If we're not the first section and we don't have a title,
    // emit a horizontal rule to separate the content
    if ($i && !$title) {
      fprintf($out, "\n* * *\n");
    }
    if ($title) {
      fprintf($out, "\n### %s\n", $title);
    }

    // If we found a declaration, emit a code block for it
    if ($decl) {
      fprintf($out, "\n```c\n%s\n```\n\n", $decl);
    }

    // and the docblock content
    fprintf($out, "\n%s\n", $extracted);
  }

  fclose($out);
}

function is_plausible_decl($text, &$title) {
  // Don't go too far into inline functions
  if (preg_match('/^\s*static inline (.*?)\{/s', $text, $matches)) {
    $text = $matches[1];
  }

  // Sanity check; if there are blank lines in there, we probably
  // got too much
  if (strpos($text, "\n\n") !== false) {
    return '';
  }

  $text = trim($text);

  // Look for what is probably the function name
  if (preg_match(',(ph_[a-z0-9_]+)\(,', $text, $matches)) {
    $title = $matches[1];
  }

  return $text;
}

// Fixup comment
// remove the C-style comment surrounds from the docblock.
// If every line begins with stars, strip those.
function extract_from_comment($comment, &$title) {
  $lines = explode("\n", $comment);
  $munged = array();

  // First line is the summary.  We render it with emphasis
  $first = array_shift($lines);
  $first = '*' . trim(substr($first, 3)) . "*\n";
  if ($first != "**\n") {
    $munged[] = $first;
  }

  // Remove closing comment bit from last line
  $last = array_pop($lines);
  $last = preg_replace(',\s+\*/$,', '', $last);
  if (strlen($last)) {
    $lines[] = $last;
  }

  // Remove leading comment decoration
  foreach ($lines as $line) {
    $line = preg_replace(',^ \*\s?,', '', $line);
    $munged[] = $line;
  }

  return implode("\n", $munged);
}

