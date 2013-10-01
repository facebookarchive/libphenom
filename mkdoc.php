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
  'include/phenom/queue.h',
));

// The final file list
$files = array_diff_key($files, $suppress);

if (!is_dir('docs')) {
  mkdir('docs');
}

ksort($files);

// First pass to extract the content
$docs = array();

// Synthesize the main page from the top-level readme
$docs['README'] = array(
  'name' => 'README',
  'title' => 'README',
  'content' => file_get_contents('README.markdown'),
  'decl_titles' => array(),
);

foreach ($files as $incname => $_) {
  process_include($incname, $docs);
}

// Build out a map of decl title to filename
$decl_map = array();
foreach ($docs as $doc) {
  foreach ($doc['decl_titles'] as $title) {
    $decl_map[$title] = $doc['title'];
  }
}

function pretty_json($data)
{
  if (defined('JSON_PRETTY_PRINT')) {
    return json_encode($data, JSON_PRETTY_PRINT);
  }

  $json = json_encode($data);

  if (getenv("PRETTY_VIA_PYTHON")) {
    $input = tmpfile();
    fwrite($input, $json);
    rewind($input);
    $proc = proc_open('python -mjson.tool',
      array(
        0 => $input,
        1 => array('pipe', 'w'),
        2 => array('file', '/dev/null', 'w')
      ),
      $pipes
    );
    $json = stream_get_contents($pipes[1]);
  }

  return $json;
}

// Save the document information
file_put_contents("docs/declmap.js",
  'var declmap = '.pretty_json($decl_map) . ";\n" .
  'var docs = '.pretty_json($docs) . ";\n"
);

exit(0);

function process_include($incname, &$docs) {
  $incfile = file_get_contents($incname);

  preg_match(',^include/phenom/(.*)\.h$,', $incname, $matches);
  $target = $matches[1];

  $md = array();

  $decl_titles = array();
  $page_title = null;

  preg_match_all(',/\*\*.*?\*/,s', $incfile, $matches, PREG_OFFSET_CAPTURE);
  foreach ($matches[0] as $i => $entry) {
    list($comment, $offset) = $entry;

    // Look ahead and see if we can find the first C-ish declaration.
    $decl = '';
    $title = '';

    $remainder = ltrim(substr($incfile, $offset + strlen($comment)));
    if (preg_match(',^[^/;]+;,s', $remainder, $declm)) {
      $decl = is_plausible_decl($declm[0], $title);
    } else if (preg_match(',^struct(.*?)\n\};,s', $remainder, $declm)) {
      $decl = is_plausible_decl($declm[0], $title);
    }
    $extracted = extract_from_comment($comment, $title);

    if ($title && !$page_title) {
      $page_title = $title;
    }

    // If we're not the first section and we don't have a title,
    // emit a horizontal rule to separate the content
    if ($i && !$title) {
      $md[] = "\n* * *\n";
    }
    if ($title) {
      $decl_titles[] = $title;

      $md[] = sprintf("\n### %s\n", $title);
    }

    // If we found a declaration, emit a code block for it
    if ($decl) {
      $md[] = sprintf("\n```c\n%s\n```\n\n", $decl);
    }

    // and the docblock content
    $md[] = sprintf("\n%s\n", $extracted);
  }

  $docs[$target] = array(
    'name' => $target,
    'title' => $target, //$page_title,
    'content' => implode('', $md),
    'decl_titles' => $decl_titles,
    'raw_content' => htmlspecialchars($incfile, ENT_QUOTES, 'utf-8'),
  );
}

function strip_comments($text) {
  $text = preg_replace(',/\*(.*?)\*/,s', '', $text);
  $text = preg_replace(",//[^\n]+,s", '', $text);
  return $text;
}

function is_plausible_decl($text, &$title) {
  // Don't go too far into inline functions
  if (preg_match('/^\s*static inline (.*?)\{/s', $text, $matches)) {
    $text = $matches[1];
  }

  // Sanity check; if there are blank lines in there, we probably
  // got too much, unless this is a struct
  if (!preg_match('/^struct/', $text) && strpos($text, "\n\n") !== false) {
    return '';
  }

  $text = trim($text);

  // Look for what is probably the function name
  $stripped = strip_comments($text);
  if (preg_match(',^typedef\s+\S+\s+\(\*(ph_[a-zA-Z0-9_]+)\),', $stripped, $matches)) {
    $title = $matches[1];
  } else if (preg_match(',^(struct\s+\S+),', $stripped, $matches)) {
    $title = $matches[1];
  } else if (preg_match(',((ph|PH)_[a-zA-Z0-9_]+)\(,', $stripped, $matches)) {
    $title = $matches[1];
  } else if (preg_match(',(struct\s+\S+),', $stripped, $matches)) {
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
  $first = preg_replace(',\*/\s*$,', '', $first);
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

