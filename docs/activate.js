
$(document).ready(function () {
  var doc = $('#doc');
  var html = marked(doc.html(), {
    highlight: function (code, lang) {
      var res = prettyPrintOne(code, lang);
      if (res) {
        return res;
      }
      return code;
    }
  });
  var p = doc.parentElement;
  doc.replaceWith($(html));

  // Populate the side nav
  var nav = $('#sidenav');
  var headings = {};

  $('h1,h2,h3,h4,h5,h6', p).each(function () {
    var id = $(this).text();

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
