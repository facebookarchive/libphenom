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

ksort($files);

// First pass to extract the content
$docs = array();
foreach ($files as $incname => $_) {
  process_include($incname, $docs);
}

foreach ($docs as $doc) {
  // Now generate markdown files
  file_put_contents("docs/$doc[name].markdown", $doc['content']);
  // and HTML
  render_html("docs/$doc[name].html", $doc, $docs);
}

exit(0);

function render_html($filename, $doc, $docs) {
  $title = htmlentities($doc['title'], ENT_QUOTES, 'utf-8');
  $html = <<<HTML
<!DOCTYPE html>
<html>
  <head>
    <title>$title</title>
    <link href="http://netdna.bootstrapcdn.com/twitter-bootstrap/2.3.2/css/bootstrap-combined.min.css" rel="stylesheet">
    <link href="style.css" rel="stylesheet"></style>
    <link href="http://netdna.bootstrapcdn.com/bootswatch/2.3.2/cerulean/bootstrap.min.css" rel="stylesheet">
  </head>
  <body data-spy="scroll" data-target=".docs-sidebar">
  <div class='navbar navbar-fixed-top'>
    <div class="navbar-inner">
      <div class='container'>
        <a class="brand" href="#">Phenom</a>
        <ul class="nav">
HTML;

  // Compute nav
  foreach ($docs as $navdoc) {
    $active = $navdoc['name'] == $doc['name'];

    if ($active) {
      $class = ' class="active"';
    } else {
      $class = '';
    }

    $navtitle = htmlentities($navdoc['title'], ENT_QUOTES, 'utf-8');
    $target = $navdoc['name'].'.html';

    $html .= "<li$class><a href=\"$target\">$navtitle</a></li>\n";
  }

  $html .= <<<HTML
        </ul>
      </div>
    </div>
  </div>

  <div class="container">
    <div class="row">
      <div class="span4 docs-sidebar">
        <ul class="nav nav-list" data-spy="affix" data-offset-top="0"></ul>
      </div>
      <div class="span8">
        <textarea id="doc">
HTML;
  $html .= $doc['content'];
  $html .= <<<HTML
        </textarea>
      </div>
    </div>
  </div>
  <script src="http://ajax.googleapis.com/ajax/libs/jquery/1.10.1/jquery.min.js"></script>
  <script src="http://netdna.bootstrapcdn.com/twitter-bootstrap/2.3.2/js/bootstrap.min.js"></script>
  <script src="marked.js"></script>
  <script src="https://google-code-prettify.googlecode.com/svn/loader/prettify.js"></script>
  <script src="activate.js"></script>
</body>
</html>
HTML;

  file_put_contents($filename, $html);
}

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

    if (preg_match(',[^/]+;,s', $incfile, $declm,
        0, $offset + strlen($comment))) {
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
  );
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

