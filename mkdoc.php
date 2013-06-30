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

// Synthesize the main page from the top-level readme
$docs['README'] = array(
  'name' => 'index',
  'title' => 'README',
  'content' => file_get_contents('README.markdown'),
  'decl_titles' => array(),
);

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
    <link href="bootstrap.min.css" rel="stylesheet">
    <link href="style.css" rel="stylesheet">
    <link href="bootstrap-responsive.min.css" rel="stylesheet">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
  </head>
  <body data-spy="scroll" data-offset="70" data-target=".docs-sidebar">
  <div class='navbar navbar-inverse navbar-fixed-top'>
    <div class="navbar-inner">
      <div class='container'>
        <button type='button' class='btn btn-navbar'
            data-toggle='collapse' data-target='.nav-collapse'>
          <span class='icon-bar'></span>
          <span class='icon-bar'></span>
          <span class='icon-bar'></span>
        </button>
        <a class="brand" href="index.html">Phenom</a>
        <div class='nav-collapse collapse'>
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
  </div>

  <div class="container">
    <div class="row">
      <div class="span4 docs-sidebar">
        <ul class="nav nav-list" id="sidenav" data-spy="affix"
          data-offset-top="0">
        </ul>
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
  <script src="jquery.min.js"></script>
  <script src="bootstrap.min.js"></script>
  <script src="marked.js"></script>
  <script src="prettify.js"></script>
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
  // got too much
  if (strpos($text, "\n\n") !== false) {
    return '';
  }

  $text = trim($text);

  // Look for what is probably the function name
  $stripped = strip_comments($text);
  if (preg_match(',(ph_[a-zA-Z0-9_]+)\(,', $stripped, $matches)) {
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

