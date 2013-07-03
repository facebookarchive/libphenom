
$(document).ready(function () {
  var raw = $('#source');
  var code = $('<pre/>');
  code.html(prettyPrintOne(raw.html(), 'c'));
  raw.replaceWith(code);

  var doc = $('#doc');
  var html = marked(doc.html(), {
    highlight: function (code, lang) {
      // allow ```none to signal that we shouldn't highlight a block at all
      if (lang != 'none') {
        var res = prettyPrintOne(code, lang);
        if (res) {
          return res;
        }
      }
      return code;
    }
  });

  // Try to auto-link stuff in the content based on our declmap
  var keys = []
  for (k in declmap) {
    keys.push(k);
  }
  var munged = []
  var regex = new RegExp('(' + keys.join('|') + ')\\(\\)');
  var m;
  while ((m = regex.exec(html)) !== null) {
    var decl = m[1];

    // Take out the text before and append
    munged.push(html.substr(0, m.index));

    // Construct a link
    munged.push('<a href="' + declmap[decl] + '#' + decl + '">' +
        decl + '()</a>');

    html = html.substr(m.index + m[0].length);
    console.log("remainder", html);
  }
  munged.push(html);
  html = munged.join('');

  var p = doc.parentElement;
  doc.replaceWith($(html));

  // Populate the side nav
  var nav = $('#sidenav');
  var headings = {};

  $('h1,h2,h3,h4,h5,h6', p).each(function () {
    var id = $(this).text().replace(/[ ]+/g, "-");

    $(this).attr('id', id);

    var text = $(this).text();
    var li = $('<li/>');
    li.append(
      $('<a/>', {
        href: '#'+id,
        title: text
      }).text(text)
    );
    nav.append(li);
  });

});
